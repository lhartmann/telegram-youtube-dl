#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <functional>
#include <chrono>
#include <optional>
#include <boost/interprocess/sync/interprocess_semaphore.hpp>
#include <tgbot/tgbot.h>
#include <boost/format.hpp>
#include <boost/process.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

// Parameters that come from environment
const char *BOT_ID    = 0;
const char *USER_IDS  = 0;
const char *PARALLEL_ENCODERS = 0;
const char *YT_USER   = 0;
const char *YT_PASS   = 0;
const char *YT_FORMAT = 0;

// Characters that may appear on a youtube video ID.
// WARNING: These mush all be shell-safe too! DO NOT USE ()!#[];/?\*&$
#define VALID_ID_CHARACTERS "0123456789qwertyuiopasdfghjklzxcvbnmQWERTYUIOPASDFGHJKLZXCVBNM-_"

using namespace std;
namespace pt = boost::property_tree;
namespace bp = boost::process;

// Helpers for access control.
std::vector<int64_t> authorized_user_ids;

bool isAuthorized(int64_t user_id) {
	for (auto id : authorized_user_ids)
		if (user_id == id) return true;
	return false;
}

// helpers to isolate a YouTube video ID from URL.
const char *yt_prefixes[] = {
	"https://www.youtube.com/watch?v=",
	"https://youtu.be/",
};

string getYoutubeVideoId(string s) {
	for (string prefix : yt_prefixes) {
		int pos = s.find(prefix);
		if (pos == string::npos) continue;
		
		pos += prefix.length();
		
		int len = s.find_first_not_of(VALID_ID_CHARACTERS, pos);
		len -= pos;
		
		return string(s, pos, len);
	}
	return "";
}

// Limit parallel encoders
boost::interprocess::interprocess_semaphore encoders(1);

uint64_t nanos() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()
        ).count();
}

// Debugging, print a ptree
void print_tree(const pt::ptree &tree, int indent=0) {
	// Trees with no children are data nodes.
	if (tree.empty()) {
		cout << "\"" << tree.data() << "\"\n";
		return;
	}
	
	// Trees with children need iterating over, but have no data.
	cout << "{\n";
	
	for (auto &child : tree) {
		for (int i=0; i<=indent; i++) cout << "  ";
		if (!child.first.empty()) 
			cout << child.first << ": ";
		print_tree(child.second, indent+1);
	}
	
	for (int i=0; i<indent; i++) cout << "  ";
	cout << "}\n";
}

std::optional<pt::ptree> download(string code, int retries=5, std::function<void(string)> log = [](string){}) {
	if (code.find_first_not_of(VALID_ID_CHARACTERS) != string::npos) 
		return {};
	
	bp::ipstream is;
	auto c = YT_USER && YT_PASS ? 
	bp::child(bp::search_path("youtube-dl"), "--print-json", 
		"-f", YT_FORMAT, 
		"-u", YT_USER, "-p", YT_PASS, "--mark-watched",
		"--", code,
		bp::std_out > is
	) : bp::child(bp::search_path("youtube-dl"), "--print-json", 
		"-f", YT_FORMAT, "--", code,
		bp::std_out > is
	);
	
	// First line of output contains JSON metadata.
	string json;
	getline(is, json);
	if (json.empty()) {
		c.terminate();
		return {};
	}
	
	pt::ptree tree;
	std::istringstream iss(json);
	pt::read_json(iss, tree);
	log("Downloading video...\n");
	
	// Give the download some time to complete
	c.wait_for(120s);
	
	// Not running likely means download completed.
	if (!c.running()) return tree;
	
	// Downlaod timed-out. terminate downloader
	c.terminate();
	
	// Abort if retries expired
	if (retries) return {};
	
	// Retry, by tail-call
	log("Retrying...\n");
	return download(code, retries-1, log);
}

void recode_cuda(string fname, std::function<void(string)> log = [](string){}) {
	// Some formats are merged into .mkv, but filename retains .mp4.
	if (!boost::filesystem::exists(fname) && fname.size()>3) 
		fname = string(fname, 0, fname.size()-3) + "mkv";
	if (!boost::filesystem::exists(fname)) {
		log("Output file not found. Fail?\n");
		return;
	}
	
	// Limit parallel encoders
	if (!encoders.try_wait()) {
		log("Encoders are busy. Queued...\n");
		encoders.wait();
	}
	
	log("Recoding...\n");
	bp::ipstream out, err;
	auto result = bp::system(
		bp::search_path("ffmpeg"), std::vector<string>{
		"-y", "-hwaccel", "cuda", "-hwaccel_output_format", "cuda",
		"-i", fname,
		"-c:v", "h264_nvenc", "-preset", "medium", "-c:a", "copy", "-r:v", "29.97",
		fname + "-recoded.mkv"},
		bp::std_out > out, bp::std_err > err
	);
	
	log(result ? "Failed.\n" : "Done.\n");
	
	encoders.post();
}

void recode_cpu(string fname, std::function<void(string)> log = [](string){}) {
	// Some formats are merged into .mkv, but filename retains .mp4.
	if (!boost::filesystem::exists(fname) && fname.size()>3) 
		fname = string(fname, 0, fname.size()-3) + "mkv";
	if (!boost::filesystem::exists(fname)) {
		log("Output file not found. Fail?\n");
		return;
	}
	
	auto pass = [=](string passno) {
		bp::ipstream out, err;
		return bp::system(
			bp::search_path("ffmpeg"), std::vector<std::string>{
			"-i", fname,
			"-y", "-c:v", "h264", "-b:v", "2M", "-c:a", "copy", "-s", "1600x900", "-r:v", "29.97",
			"-passlogfile", fname, "-pass", passno,
			fname + "-recoded.mkv"},
			bp::std_out > out, bp::std_err > err
		);
	};
	
	if (!encoders.try_wait()) {
		log("Encoders are busy. Queued...\n");
		encoders.wait();
	}
	
	log("Recoding, first pass...\n");
	if (pass("1")) {
		log("Failed!\n");
		encoders.post();
		return;
	}
	
	log("Recoding, second pass...\n");
	if (pass("2")) {
		log("Failed!\n");
		encoders.post();
		return;
	}
	
	log("Done!\n");
	
	encoders.post();
}

void onMessage(TgBot::Bot &bot, TgBot::Message::Ptr message) {
	// Refuse unkonwn users.
	if (!isAuthorized(message->from->id)) {
		cout << boost::format("Refused message from %s (%d).\n") % message->from->firstName % message->from->id;
		bot.getApi().sendMessage(message->chat->id,
			(boost::format("Sorry, %s. I'm not allowed to talk to strangers.") % message->from->firstName).str()
		);
		return;
	}

	// Do not reply to commands
	if (StringTools::startsWith(message->text, "/")) {
		return;
	}

	// 
	string video_id = getYoutubeVideoId(message->text);
	if (!video_id.empty()) {
		string status = "Downloading information...\n";
		
		cout << "Received youtube video url for ID=" << video_id << endl;
		auto sent = bot.getApi().sendMessage(message->chat->id, status, false, message->messageId);
		auto log = [status, &bot, sent, t0=nanos()](string text) mutable {
			status += str(boost::format("[%3.3f] %s") % ((nanos()-t0)/1e9) % text);
			sent = bot.getApi().editMessageText(status, sent->chat->id, sent->messageId);
		};
		
		auto tree = download(video_id, 5, log);
		if (!tree) {
			log("Download failed.\n");
			return;
		}
		log("Download completed.\n");
		
		// Recode for ye-olde Raspberry Pi 1 on a 1600x900 monitor.
		auto fname = tree->get_optional<string>("_filename");
		if (!fname) {
			log("Filename not provided, skipping recode.\n");
			return;
		}
		
		thread(recode_cpu, *fname, log).detach();
		return;
	}
	
	bot.getApi().sendMessage(message->chat->id, "Sorry, what?\n");
}

const char *env(const char *name, const char *def=0) {
	const char *r = getenv(name);
	if (r) return r;
	return def;
}

int main() {
	BOT_ID    = env("BOT_ID");
	USER_IDS  = env("USER_IDS");
	PARALLEL_ENCODERS = getenv("PARALLEL_ENCODERS");
	YT_USER   = env("YT_USER");
	YT_PASS   = env("YT_PASS");
	YT_FORMAT = env("YT_FORMAT", "bestvideo[height<=1080]+bestaudio");
	
	// Sanity checks
	if (!BOT_ID || !USER_IDS) {
		cout << "Please set BOT_ID and USER_IDS prior to running this program." << endl;
		return 1;
	}
	
	// How many encoders are allowed in parallel
	if (PARALLEL_ENCODERS) {
		int n = max(1, atoi(PARALLEL_ENCODERS));
		cout << "Using " << n << " parallel encoders." << endl;
		while (--n) encoders.post();
	}
	
	// Youtube login (optional)
	if (YT_USER && YT_PASS) {
		cout << "Using " << YT_USER << " youtube's account" << endl;
	}
	
	TgBot::Bot bot(BOT_ID);
	unsetenv("BOT_ID");

	{ // Load users list
		istringstream users(USER_IDS);
		for (uint64_t uid; users >> uid;) 
			authorized_user_ids.push_back(uid);
	}
	
	bot.getEvents().onCommand("start", [&bot](TgBot::Message::Ptr message) {
		bot.getApi().sendMessage(message->chat->id, "Hi!");
	});
	
	bot.getEvents().onAnyMessage(
		[&](auto a) {
			onMessage(bot, a); 
		}
	);
	
	try {
		cout << "Bot username: " << bot.getApi().getMe()->username << endl;
		// Poll server for updates, one at a time.
		TgBot::TgLongPoll longPoll(bot, 1);
		while (true) {
			cout << "Long poll started..." << endl;
			longPoll.start();
		}
	} catch (TgBot::TgException& e) {
		cerr << "error: " << e.what() << endl;
	}
	return 0;
}
