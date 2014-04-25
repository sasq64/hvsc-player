
#include <VicePlugin/VicePlugin.h>
#include <audioplayer/audioplayer.h>

#include <ChipPlugin.h>
#include <ChipPlayer.h>
#include <SongDb.h>

#include <mutex>
#include <coreutils/utils.h>
#include <webutils/webgetter.h>
#include <grappix/grappix.h>
#include <algorithm>
#include <functional>

#ifdef EMSCRIPTEN
#include <emscripten.h>
#endif

using namespace utils;
using namespace std;
using namespace grappix;
using namespace chipmachine;
using namespace std::placeholders;

static const int bufSize = 32768;
static double percent = 20;

string setSearchString;
bool newSS = false;

int playIndex = -1;
extern "C" {

void play_index(int i) {
	playIndex = i;
}

void set_searchstring(char *s) {
	setSearchString = s;
	newSS = true;
}

}


struct App {
	shared_ptr<ChipPlayer> player;
 	unique_ptr<AudioPlayer> audioplayer;
	TileSet font;
	TileArray ta;
	TileLayer tiles;
	unique_ptr<VicePlugin> vicePlugin;
	int asciiToCbm[256];

	WebGetter webGetter;
	unique_ptr<WebGetter::Job> sidJob;
	SongDatabase db;
	IncrementalQuery query;

	mutex m;
	bool audio = true;
	int cursorx = 1;
	int marker = -1;
	int lastMarker = -1;
	int toPlay = -1;
	int currentSong = -1;
	int maxSongs = 0;
	int lastSong = -1;
	int scrollpos = 0;
	int delay = 0;


	App() : ta(40,25), tiles(16*40, 16*25, 16, 16, font, ta), webGetter("files"), db { "data/hvsc.db" } {

		db.generateIndex();
#ifndef EMSCRIPTEN
		webGetter.setBaseURL("http://swimsuitboys.com/hvsc/");
#endif

		for(int i=0; i<256; i++) {
			if(i < 0x20)
				asciiToCbm[i] = 0;
			else if(i <= '?')
				asciiToCbm[i] = i;
			else if(i == '@')
				asciiToCbm[i] = 0;
			else if(i <= 'Z')
				asciiToCbm[i] = i - 'A' + 65;
			else if(i <= 'z')
				asciiToCbm[i] = i - 'a' + 1;
			else if(i <= 97)
				asciiToCbm[i] = 0;
			else
				asciiToCbm[i] = 0;
		}

		vicePlugin = make_unique<VicePlugin>("data/c64");

		auto bm = image::load_png("data/c64/c64.png");
		// Replace all non zero pixels with constant color
		replace_if(bm.begin(), bm.end(), bind(not_equal_to<uint32_t>(), _1, 0), 0xffb55e6c);

		for(const auto &b : bm.split(8,8))
			font.add(b);

		ta.fill(0x20);

		audioplayer = make_unique<AudioPlayer>([&](int16_t *ptr, int size) {
			if(player) {
				lock_guard<mutex> guard(m);

				auto now = getms();//emscripten_get_now();
				player->getSamples(ptr, size);
				auto t = getms() - now;

				double p = (t * 100) / (size / 2.0 / 44.1);
				percent = percent * 0.8 + p * 0.2;
			} else {
				memset(ptr, 0, size*2);
			}
		});

	    sidJob = unique_ptr<WebGetter::Job>(webGetter.getURL("C64Music/MUSICIANS/G/Galway_Martin/Ocean_Loader_2.sid"));
	   	query = db.find();

		print("@", 0, 4);

	}

	void update(int d) {

		if(newSS) {
			query.setString(setSearchString);
			newSS = false;
		}

		if(playIndex >= 0) {
			toPlay = playIndex;
			playIndex = -1;
		}

		if(sidJob && sidJob->isDone()) {
			ta.fill(0x20, 0, 0, 0, 3);
			lock_guard<mutex> guard(m);

			player = nullptr;
			player = shared_ptr<ChipPlayer>(vicePlugin->fromFile(sidJob->getFile()));
			print(player->getMeta("title"), 0, 0);
			print(player->getMeta("composer"), 0, 1);
			print(player->getMeta("copyright"), 0, 2);
			maxSongs = player->getMetaInt("songs");
			lastSong = -1;
			currentSong = player->getMetaInt("startsong");

		    sidJob = nullptr;
		}

		auto k = screen.get_key();
		bool updateResult = query.newResult();
		if(updateResult) {
			marker = scrollpos = 0;
		}

		if(delay) {
			if(delay == 1) {
				if(screen.key_pressed(Window::UP))
					marker--;
				else if(screen.key_pressed(Window::DOWN))
					marker++;
				else
					delay = 0;
			}
			else delay--;
		} 


		if(k != Window::NO_KEY) {
			LOGD("Pressed 0x%02x", k);
			switch((int)k) {
			case 0x08:
			case Window::BACKSPACE:
				query.removeLast();
				updateResult = true;
				break;
			case Window::ESCAPE:
			case Window::F1:
			case 255:
				query.clear();
				updateResult = true;
				break;
			case Window::UP:
				if(!delay) {
					marker--;
					delay = 4;
				}
				break;
			case Window::DOWN:
				if(!delay) {
					marker++;
					delay = 4;
				}
				break;
			case Window::PAGEUP:
				marker-=(ta.height()-5);
				break;
			case Window::PAGEDOWN:
				marker+=(ta.height()-5);
				break;
			case Window::LEFT:
				if(currentSong > 0)
					player->seekTo(--currentSong);
				break;
			case Window::RIGHT:
				if(currentSong+1 < maxSongs)
					player->seekTo(++currentSong);
				break;
			case Window::ENTER:
			case 0x0d:
			case 0x0a:
				toPlay = marker;
				break;
			default:
				break;
			}

			if(k < 0x100) {
				if(isalnum(k) || k == ' ') {
					query.addLetter(tolower(k));
					updateResult = true;
				}
			}
			//if(k == Window::ENTER || k == Window::SPACE) {
			//	SDL_PauseAudio(audio ? 1 : 0);
			//	audio = !audio;
			//}
		}
		int h = ta.height()-5;

		if(marker < 0)
			marker = 0;
		if(marker >= query.numHits())
			marker = query.numHits()-1;
		//if(marker >= tiles.height())
		//	marker = tiles.height()-1;

		while(marker >= scrollpos + h) {
			scrollpos++;
			updateResult = true;
		}
		while(marker > 0  && marker < scrollpos) {
			scrollpos--;
			updateResult = true;
		}

		if(updateResult) {
			ta.fill(0x20, 1, 4, 0, 1);
			print(query.getString(), 1, 4);
			ta.fill(0X20, 1, 5);
			const auto &results = query.getResult(scrollpos, h);
			LOGD("%d %d -> %d", scrollpos, h, results.size());
			int i=0;
			for(const auto &r : results) {
				auto p = split(r, "\t");
				if(p.size() < 3) {
					LOGD("Illegal result line '%s' -> [%s]", r, p);
				} else {
					//int index = atoi(p[2].c_str());
					//int fmt = db.getFormat(index);
					//int color = Console::WHITE;
					print(format("%s / %s", p[0], p[1]), 1, i+5);
					//console.put(1, i+3, utf8_encode(format("%s - %s", p[1], p[0])), color);
				}
				i++;
				//if(i >= h-3)
				//	break;
			}
		}

		if(currentSong != lastSong) {
			print(format("SONG %02d/%02d", currentSong+1, maxSongs), ta.width()-10, 0);
			lastSong = currentSong;
		}

		if(toPlay >= 0) {
			string r = query.getFull(toPlay);
			toPlay = -1;
			LOGD("RESULT: %s", r);
			auto p  = utils::split(r, "\t");
			for(size_t i = 0; i<p[2].length(); i++) {
				if(p[2][i] == '\\')
					p[2][i] = '/';
			}
			LOGD("Playing '%s'", p[2]);
			sidJob = unique_ptr<WebGetter::Job>(webGetter.getURL(p[2])); //  + 
		}


		if(marker != lastMarker) {
			if(lastMarker >= 0)
				print(" ", 0, lastMarker);
			if(marker >=0 )
				print(">", 0, marker+5-scrollpos);
			lastMarker = marker+5-scrollpos;
		}

		screen.clear(0x352879);
		print(format("%03d%", (int)percent), 36, 2);
		tiles.render(screen);
		screen.flip();
	}

	void print(const std::string &text, int x, int y) {
		int i = x + y * ta.width();
		int maxi = (y+1) * ta.width();
		int total = ta.width() * ta.height();
		for(const auto &c : text) {
			if(i >= maxi || i >= total)
				break;
			ta[i++] = asciiToCbm[c & 0xff];
		}
	}
};

void runMainLoop(int d) {
	static App app;
	app.update(d);
}

int main() {
	LOGD("main");	
	screen.open(640, 400, false);
	LOGD("Screen is open");

	screen.render_loop(runMainLoop, 20);
	return 0;
}