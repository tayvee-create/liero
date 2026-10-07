#include "netController.hpp"

#include "localController.hpp"
#include "stats_presenter.hpp"
#include "../gfx.hpp"
#include "../sfx.hpp"
#include "../keys.hpp"
#include "../replay.hpp"
#include "../weapsel.hpp"
#include "../worm.hpp"
#include "../viewport.hpp"
#include "../spectatorviewport.hpp"
#include "../filesystem.hpp"
#include "../console.hpp"
#include "../gfx/blit.hpp"

#include <SDL.h>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <unistd.h>

uint32_t fastGameChecksum(Game& game); // replay.cpp

NetAutoStart netAutoStart;

static NetController* activeNet = nullptr;

void netShutdown()
{
	if (activeNet)
		activeNet->notifyQuit();
}

namespace
{

int const ticksPerSecond = 70;
int const defaultInputDelay = 3;          // ~43 ms, plenty for a home network
int const checksumInterval = ticksPerSecond;
int const stallMessageTicks = ticksPerSecond / 2;
int const connectionTimeoutTicks = ticksPerSecond * 10;

// Temporarily makes `seeded` the global gfx.rand, which weapon selection draws from.
struct RandSwap
{
	RandSwap(Rand& r) : r(r) { std::swap(gfx.rand, r); }
	~RandSwap() { std::swap(gfx.rand, r); }
	Rand& r;
};

}

NetController::NetController(std::unique_ptr<Game> gameInit, std::unique_ptr<net::Connection> connInit,
	bool isHost, uint32_t syncSeed, int inputDelayInit)
: game(std::move(gameInit))
, conn(std::move(connInit))
, state(StateInitial)
, fadeValue(0)
, goingToMenu(false)
, localIdx(isHost ? 1 : 0)
, remoteIdx(isHost ? 0 : 1)
, inputDelay(inputDelayInit)
, simFrame(0)
, nextLocalFrame(inputDelayInit)
, desynced(false)
, aborted(false)
, stalledTicks(0)
, silentTicks(0)
, escapeTimer(0)
{
	syncRand.seed(syncSeed);
	activeNet = this;

	// The first frames have no input from either side
	for (int f = 0; f < inputDelay; ++f)
	{
		inputs[0][f];
		inputs[1][f];
	}

	// Same layout as a local game: joiner on the left, host on the right
	game->clearViewports();
	game->worms[0]->statsX = 0;
	game->worms[1]->statsX = 218;
	game->addViewport(new Viewport(gvl::rect(0, 0, 158, 158), game->worms[0]->index, 504, 350));
	game->addViewport(new Viewport(gvl::rect(160, 0, 158+160, 158), game->worms[1]->index, 504, 350));
	game->addSpectatorViewport(new SpectatorViewport(gvl::rect(0, 0, 504 + 68, 350), 504, 350));
}

NetController::~NetController()
{
	if (activeNet == this)
		activeNet = nullptr;
	endRecord();
	notifyQuit();
}

void NetController::notifyQuit()
{
	if (conn && conn->isOpen() && running())
	{
		conn->send(net::MsgQuit, net::Buffer());
		conn->pump();
	}
}

void NetController::onKey(int key, bool keyState)
{
	if (key == DkEscape)
	{
		if (!keyState || goingToMenu)
			return;
		if (state == StateGameEnded || aborted)
		{
			fadeValue = 31;
			goingToMenu = true;
		}
		else if (escapeTimer > 0)
		{
			conn->send(net::MsgQuit, net::Buffer());
			abort("");
		}
		else
			escapeTimer = ticksPerSecond * 2;
		return;
	}

	if (state != StateWeaponSelection && state != StateGame)
		return;

	// Each player has their own Mac, so everyone gets the full-keyboard scheme:
	// arrows + X fire, C change, Z jump, V dig
	for (int c = 0; c < WormSettings::MaxControlEx; ++c)
	{
		if (onePlayerControls[c] == uint32_t(key))
		{
			pendingEvents.push_back(uint8_t(c | (keyState ? 0x80 : 0)));
			return;
		}
	}

	// Either player's configured keys on this keyboard also work
	bool ext = gfx.settings->extensions;
	std::size_t maxControl = ext ? WormSettings::MaxControlEx : WormSettings::MaxControl;
	for (int p = 0; p < 2; ++p)
	{
		WormSettings& ws = *gfx.settings->wormSettings[p];
		uint32_t* controls = ext ? ws.controlsEx : ws.controls;
		for (std::size_t c = 0; c < maxControl; ++c)
		{
			if (controls[c] == uint32_t(key))
			{
				pendingEvents.push_back(uint8_t(c | (keyState ? 0x80 : 0)));
				return;
			}
		}
	}
}

// Mirrors LocalController::onKey, but driven by synchronized events
void NetController::applyEvent(Worm& worm, uint8_t ev)
{
	Worm::Control control = Worm::Control(ev & 0x7f);
	bool keyState = (ev & 0x80) != 0;

	worm.cleanControlStates.set(control, keyState);
	if (control < Worm::MaxControl)
		worm.setControlState(control, keyState);

	if (worm.cleanControlStates[WormSettings::Dig])
	{
		worm.press(Worm::Left);
		worm.press(Worm::Right);
	}
	else
	{
		if (!worm.cleanControlStates[Worm::Left])
			worm.release(Worm::Left);
		if (!worm.cleanControlStates[Worm::Right])
			worm.release(Worm::Right);
	}
}

void NetController::unfocus()
{
	if (replay)
		replay->unfocus();
	if (state == StateWeaponSelection)
		ws->unfocus();
}

void NetController::focus()
{
	if (state == StateGameEnded || aborted)
	{
		goingToMenu = true;
		fadeValue = 0;
		return;
	}
	if (state == StateWeaponSelection)
		ws->focus();
	if (replay)
		replay->focus();
	if (state == StateInitial)
		changeState(StateWeaponSelection);
	game->focus(gfx.playRenderer);
	game->focus(gfx.singleScreenRenderer);
	goingToMenu = false;
	fadeValue = 0;
}

void NetController::abort(std::string const& reason)
{
	if (aborted)
		return;
	aborted = true;
	abortReason = reason;
	endRecord();
	if (conn)
		conn->pump();
	goingToMenu = true;
	fadeValue = std::min(fadeValue, 31);
}

void NetController::pumpNetwork()
{
	bool alive = conn->pump();

	net::MsgType type;
	net::Buffer msg;
	while (conn->nextMessage(type, msg))
	{
		silentTicks = 0;
		switch (type)
		{
		case net::MsgInput:
		{
			uint32_t frame = msg.getU32();
			uint8_t count = msg.getU8();
			// Only accept frames we can still use and that a real peer could be sending
			if (frame < simFrame || frame > simFrame + 10 * ticksPerSecond)
				break;
			std::vector<uint8_t>& evs = inputs[remoteIdx][frame];
			for (int i = 0; i < count; ++i)
			{
				uint8_t ev = msg.getU8();
				if ((ev & 0x7f) < WormSettings::MaxControlEx)
					evs.push_back(ev);
			}
			break;
		}
		case net::MsgCheck:
		{
			uint32_t frame = msg.getU32();
			remoteChecks[frame] = msg.getU32();
			break;
		}
		case net::MsgQuit:
			if (state != StateGameEnded)
				abort("YOUR OPPONENT LEFT THE GAME");
			break;
		default:
			break;
		}
	}

	// Compare checksums once both sides have reported a frame
	for (auto i = remoteChecks.begin(); i != remoteChecks.end(); )
	{
		auto l = localChecks.find(i->first);
		if (l == localChecks.end())
		{
			++i;
			continue;
		}
		if (netAutoStart.bot)
			std::fprintf(stderr, "net: frame %u local %08x remote %08x %s\n", i->first, l->second, i->second,
				l->second == i->second ? "ok" : "DESYNC");
		if (l->second != i->second && !desynced)
		{
			desynced = true;
			Console::writeWarning("Network game desynced at frame " + std::to_string(i->first));
		}
		localChecks.erase(l);
		i = remoteChecks.erase(i);
	}

	if (!alive && !aborted && state != StateGameEnded)
		abort("CONNECTION TO YOUR OPPONENT WAS LOST");
}

void NetController::botInput()
{
	// Hold keys for a while at random; fire a lot so the match actually progresses
	static uint32_t held = 0;
	for (int c = 0; c < Worm::MaxControl; ++c)
	{
		int chance = (c == Worm::Fire) ? 8 : 25;
		if (std::rand() % chance == 0)
		{
			held ^= 1u << c;
			pendingEvents.push_back(uint8_t(c | ((held >> c) & 1 ? 0x80 : 0)));
		}
	}
}

void NetController::sendLocalInput()
{
	uint32_t frame = nextLocalFrame++;

	// Event lists are tiny; cap at 255 per frame for the wire format
	if (pendingEvents.size() > 255)
		pendingEvents.resize(255);

	net::Buffer msg;
	msg.u32(frame);
	msg.u8(uint8_t(pendingEvents.size()));
	msg.bytes(pendingEvents.data(), pendingEvents.size());
	conn->send(net::MsgInput, msg);

	inputs[localIdx][frame].swap(pendingEvents);
	pendingEvents.clear();
}

void NetController::stepFrame()
{
	for (int idx = 0; idx < 2; ++idx)
	{
		auto i = inputs[idx].find(simFrame);
		for (uint8_t ev : i->second)
			applyEvent(*game->worms[idx], ev);
		inputs[idx].erase(i);
	}

	if (state == StateWeaponSelection)
	{
		RandSwap swap(syncRand);
		if (ws->processFrame())
			changeState(StateGame);
	}
	else if (state == StateGame)
	{
		if (replay)
		{
			try
			{
				replay->recordFrame();
			}
			catch (std::runtime_error& e)
			{
				Console::writeWarning(std::string("Error recording replay frame: ") + e.what());
				replay.reset();
			}
		}

		game->processFrame();

		if (simFrame % checksumInterval == 0)
		{
			uint32_t sum = fastGameChecksum(*game);
			localChecks[simFrame] = sum;
			net::Buffer msg;
			msg.u32(simFrame);
			msg.u32(sum);
			conn->send(net::MsgCheck, msg);
		}

		if (game->isGameOver())
			changeState(StateGameEnded);
	}

	++simFrame;
}

bool NetController::process()
{
	if (escapeTimer > 0)
		--escapeTimer;

	if (!aborted && state != StateGameEnded)
	{
		pumpNetwork();
		++silentTicks;
		if (!aborted && silentTicks > connectionTimeoutTicks)
			abort("CONNECTION TO YOUR OPPONENT TIMED OUT");
	}

	if (!aborted && (state == StateWeaponSelection || state == StateGame))
	{
		if (netAutoStart.bot)
			botInput();

		bool stepped = false;
		for (int step = 0; step < 2; ++step)
		{
			while (nextLocalFrame < simFrame + inputDelay)
				sendLocalInput();

			if (!inputs[0].count(simFrame) || !inputs[1].count(simFrame))
				break;

			stepFrame();
			stepped = true;

			if (state != StateWeaponSelection && state != StateGame)
				break;

			// Only run a second frame this tick if we have fallen behind the other side
			if (!inputs[remoteIdx].count(simFrame + inputDelay))
				break;
		}
		stalledTicks = stepped ? 0 : stalledTicks + 1;
	}
	else if (state == StateGameEnded)
	{
		// The match is decided; let the end play out locally
		game->processFrame();
	}

	if (goingToMenu)
	{
		if (fadeValue > 0)
			fadeValue -= 1;
		else
		{
			if (aborted)
			{
				if (!abortReason.empty())
					gfx.infoBox(abortReason);
			}
			else if (state == StateGameEnded)
			{
				endRecord();
				game->statsRecorder->finish(*game);
				presentStats(static_cast<NormalStatsRecorder&>(*game->statsRecorder), *game);
			}
			conn->close();
			return false;
		}
	}
	else if (fadeValue < 33)
	{
		fadeValue += 1;
	}

	return true;
}

void NetController::draw(Renderer& renderer, bool useSpectatorViewports)
{
	if (state == StateWeaponSelection)
		ws->draw(renderer, state, useSpectatorViewports);
	else
		game->draw(renderer, state, useSpectatorViewports);
	renderer.fadeValue = fadeValue;

	if (useSpectatorViewports)
		return;

	std::string message;
	if (escapeTimer > 0)
		message = "PRESS ESC AGAIN TO LEAVE THE MATCH";
	else if (!aborted && stalledTicks > stallMessageTicks)
		message = "WAITING FOR OPPONENT...";
	else if (desynced)
		message = "WARNING: GAMES OUT OF SYNC";

	if (!message.empty())
	{
		Font& font = game->common->font;
		int width = font.getDims(message);
		int x = 160 - width / 2;
		int y = 80;
		drawRoundedBox(renderer.bmp, x - 2, y - 1, 0, 7, width);
		font.drawText(renderer.bmp, message, x, y, 50);
	}
}

void NetController::changeState(GameState newState)
{
	if (state == newState)
		return;

	if (state == StateWeaponSelection)
		ws->finalize();

	if (newState == StateWeaponSelection)
	{
		RandSwap swap(syncRand);
		ws.reset(new WeaponSelection(*game));
		ws->netLocalWorm = localIdx;
	}
	else if (newState == StateGame)
	{
		for (auto* worm : game->worms)
			worm->lives = game->settings->lives;

		if (gfx.settings->extensions && gfx.settings->recordReplays)
			startRecording();

		game->startGame();
	}
	else if (newState == StateGameEnded)
	{
		if (!goingToMenu)
		{
			fadeValue = 180;
			goingToMenu = true;
		}
	}

	if (state == StateWeaponSelection)
	{
		fadeValue = 33;
		ws.reset();
	}

	state = newState;
}

void NetController::startRecording()
{
	try
	{
		std::time_t ticks = std::time(0);
		char buf[64];
		std::strftime(buf, sizeof(buf), "%Y-%m-%d %H.%M.%S", std::localtime(&ticks));

		std::string playerNames = " ";
		for (std::size_t i = 0; i < 2; ++i)
		{
			if (i > 0)
				playerNames.push_back('-');
			std::string const& name = game->worms[i]->settings->name;
			for (std::size_t c = 0, chars = 0; c < name.size() && chars < 4; ++c, ++chars)
			{
				unsigned char ch = (unsigned char)name[c];
				if (std::isalnum(ch))
					playerNames.push_back(ch);
			}
		}

		auto node = gfx.getConfigNode() / "Replays" / (std::string(buf) + " NET" + playerNames + ".lrp");
		replay.reset(new ReplayWriter(node.toSink()));
		replay->beginRecord(*game);
	}
	catch (std::runtime_error& e)
	{
		// Recording is a nice-to-have; never abort a network match over it
		Console::writeWarning(std::string("Could not record replay: ") + e.what());
		replay.reset();
	}
}

void NetController::endRecord()
{
	replay.reset();
}

bool NetController::running()
{
	return !aborted && state != StateGameEnded && state != StateInitial;
}

Level* NetController::currentLevel()
{
	return &game->level;
}

Game* NetController::currentGame()
{
	return game.get();
}

void NetController::swapLevel(Level& newLevel)
{
	game->level.swap(newLevel);
}

//
// Host / join screens
//

namespace
{

std::string lastJoinIp;

void drawSetupScreen(std::vector<std::string> const& lines)
{
	Common& common = *gfx.common;
	Bitmap& bmp = gfx.playRenderer.bmp;

	gfx.playRenderer.pal = common.exepal;
	gfx.playRenderer.fadeValue = 32;
	fill(bmp, 0);

	int y = 100 - int(lines.size()) * 11 / 2;
	for (std::size_t i = 0; i < lines.size(); ++i)
	{
		if (!lines[i].empty())
			common.font.drawCenteredText(bmp, lines[i], 160, y, i == 0 ? 50 : 6);
		y += 11;
	}

	gfx.primaryRenderer = &gfx.playRenderer;
	gfx.flip();
	gfx.process();
}

std::string tempGamePath()
{
	char const* tmp = std::getenv("TMPDIR");
	std::string dir = tmp ? tmp : "/tmp/";
	if (dir.empty() || dir.back() != '/')
		dir += '/';
	return dir + "liero-net-" + std::to_string(getpid()) + ".lrp";
}

std::unique_ptr<Game> loadGame(std::string const& path)
{
	ReplayReader reader(FsNode(path).toSource());
	return reader.beginPlayback(gfx.common,
		gvl::shared_ptr<SoundPlayer>(new DefaultSoundPlayer(*gfx.common)));
}

// Waits for one message, keeping the window alive. Returns false on cancel/disconnect/timeout.
bool waitForMessage(net::Connection& conn, net::MsgType& type, net::Buffer& msg,
	std::vector<std::string> const& screen)
{
	for (int tick = 0; tick < ticksPerSecond * 15; ++tick)
	{
		bool alive = conn.pump();
		if (conn.nextMessage(type, msg))
			return true;
		if (!alive)
			return false;
		drawSetupScreen(screen);
		if (gfx.testSDLKeyOnce(SDL_SCANCODE_ESCAPE))
			return false;
	}
	return false;
}

int filterIp(int k)
{
	return (std::isdigit(k) || k == '.') ? k : 0;
}

void reject(net::Connection& conn, std::string const& reason)
{
	net::Buffer msg;
	msg.str(reason);
	conn.send(net::MsgReject, msg);
	for (int i = 0; i < 10 && !conn.outbuf.empty(); ++i)
	{
		conn.pump();
		SDL_Delay(10);
	}
}

std::unique_ptr<Controller> hostGame()
{
	Settings& settings = *gfx.settings;
	std::string myName = settings.wormSettings[0]->name;

	net::Host host;
	std::string error;
	if (!host.start(error))
	{
		gfx.infoBox(error);
		return nullptr;
	}

	std::string ip = net::localAddress();
	if (ip.empty())
		ip = "UNKNOWN - CHECK YOUR WI-FI";

	std::unique_ptr<net::Connection> conn;
	while (!conn)
	{
		host.answerDiscovery(myName);
		int fd = host.accept();
		if (fd >= 0)
		{
			conn.reset(new net::Connection(fd));
			break;
		}
		drawSetupScreen({
			"HOST NETWORK GAME",
			"",
			"YOUR IP ADDRESS: " + ip,
			"",
			"WAITING FOR YOUR OPPONENT TO JOIN...",
			"(THEY CHOOSE JOIN NETWORK GAME)",
			"",
			"YOU ARE THE RIGHT WORM",
			"MATCH SETUP (F7) IS USED FOR THIS GAME",
			"",
			"ESC: CANCEL"});
		if (gfx.testSDLKeyOnce(SDL_SCANCODE_ESCAPE))
			return nullptr;
	}
	host.stop();

	net::MsgType type;
	net::Buffer hello;
	if (!waitForMessage(*conn, type, hello, {"HOST NETWORK GAME", "", "OPPONENT CONNECTING..."})
	 || type != net::MsgHello)
	{
		gfx.infoBox("OPPONENT DID NOT RESPOND");
		return nullptr;
	}

	uint16_t version = hello.getU16();
	std::string tc = hello.getStr();
	std::string name = hello.getStr().substr(0, 20); // Same limit as the name editor
	int rgb[3];
	for (int i = 0; i < 3; ++i)
		rgb[i] = hello.getU8() & 63;
	uint32_t weapons[5];
	for (int i = 0; i < 5; ++i)
		weapons[i] = hello.getU8();

	if (version != net::protocolVersion)
	{
		reject(*conn, "DIFFERENT GAME VERSIONS - USE THE SAME APP");
		gfx.infoBox("OPPONENT HAS A DIFFERENT GAME VERSION");
		return nullptr;
	}
	if (tc != settings.tc)
	{
		reject(*conn, "HOST USES TC \"" + settings.tc + "\" - PICK IT UNDER TC");
		gfx.infoBox("OPPONENT USES A DIFFERENT TC: " + tc);
		return nullptr;
	}

	// Build the initial game from our settings: the joiner is the left worm (0) and
	// we, the host, are the right worm (1) using our own left player profile.
	// Our settings are changed only while serializing, then restored.
	struct SavedWorm
	{
		uint32_t controller;
		std::string name;
		bool randomName;
		int rgb[3];
		uint32_t weapons[5];
	};
	auto save = [](WormSettings& ws) {
		SavedWorm sw;
		sw.controller = ws.controller;
		sw.name = ws.name;
		sw.randomName = ws.randomName;
		std::copy(ws.rgb, ws.rgb + 3, sw.rgb);
		std::copy(ws.weapons, ws.weapons + 5, sw.weapons);
		return sw;
	};
	auto apply = [](WormSettings& ws, SavedWorm const& sw) {
		ws.controller = sw.controller;
		ws.name = sw.name;
		ws.randomName = sw.randomName;
		std::copy(sw.rgb, sw.rgb + 3, ws.rgb);
		std::copy(sw.weapons, sw.weapons + 5, ws.weapons);
	};

	WormSettings& ws0 = *settings.wormSettings[0];
	WormSettings& ws1 = *settings.wormSettings[1];
	SavedWorm saved[2] = {save(ws0), save(ws1)};

	SavedWorm me = saved[0];
	me.controller = 0;
	me.randomName = false;
	SavedWorm opponent;
	opponent.controller = 0;
	opponent.name = name;
	opponent.randomName = false;
	std::copy(rgb, rgb + 3, opponent.rgb);
	for (int i = 0; i < 5; ++i)
		opponent.weapons[i] = weapons[i] <= 40 ? weapons[i] : 0;

	apply(ws0, opponent);
	apply(ws1, me);

	std::string path = tempGamePath();
	std::string buildError;
	try
	{
		LocalController setup(gfx.common, gfx.settings);
		Level level(*gfx.common);
		level.generateFromSettings(*gfx.common, settings, gfx.rand);
		setup.swapLevel(level);

		ReplayWriter writer(FsNode(path).toSink());
		writer.beginRecord(setup.game);
	}
	catch (std::exception& e)
	{
		buildError = e.what();
	}

	apply(ws0, saved[0]);
	apply(ws1, saved[1]);

	if (!buildError.empty())
	{
		reject(*conn, "HOST COULD NOT START THE GAME");
		gfx.infoBox("COULD NOT START GAME: " + buildError);
		return nullptr;
	}

	std::ifstream in(path, std::ios::binary);
	std::string blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	in.close();

	uint32_t seed = gfx.rand();

	net::Buffer start;
	start.u32(seed);
	start.u8(defaultInputDelay);
	start.str(blob);
	conn->send(net::MsgStart, start);

	// Load from the same bytes we sent so both sides start bit-identical
	std::unique_ptr<Game> game;
	try
	{
		game = loadGame(path);
	}
	catch (std::exception& e)
	{
		std::remove(path.c_str());
		gfx.infoBox(std::string("COULD NOT LOAD GAME: ") + e.what());
		return nullptr;
	}
	std::remove(path.c_str());

	return std::unique_ptr<Controller>(
		new NetController(std::move(game), std::move(conn), true, seed, defaultInputDelay));
}

std::unique_ptr<Controller> joinGame()
{
	Settings& settings = *gfx.settings;

	net::Discovery discovery;
	discovery.start();

	std::unique_ptr<net::Connection> conn(new net::Connection);
	enum { Searching, Connecting, Waiting } phase = Searching;
	std::string target, status;
	uint32_t phaseStart = 0;
	uint32_t lastBroadcast = 0;
	std::string autoIp;
	std::swap(autoIp, netAutoStart.ip);

	auto connectTo = [&](std::string const& ip, std::string const& label) {
		target = ip;
		if (conn->connectTo(ip, net::gamePort))
		{
			phase = Connecting;
			phaseStart = SDL_GetTicks();
			status = "CONNECTING TO " + label + "...";
		}
		else
			status = "\"" + ip + "\" IS NOT A VALID IP ADDRESS";
	};

	if (!autoIp.empty())
		connectTo(autoIp, autoIp);

	while (true)
	{
		uint32_t now = SDL_GetTicks();

		if (phase == Searching)
		{
			if (now - lastBroadcast > 500)
			{
				discovery.broadcast();
				lastBroadcast = now;
			}
			net::FoundHost found;
			if (discovery.poll(found))
				connectTo(found.ip, found.name.empty() ? found.ip : found.name + " (" + found.ip + ")");
		}
		else if (phase == Connecting)
		{
			int r = conn->pollConnect();
			if (r > 0)
			{
				net::Buffer hello;
				hello.u16(net::protocolVersion);
				hello.str(settings.tc);
				WormSettings& me = *settings.wormSettings[0];
				hello.str(me.name);
				for (int i = 0; i < 3; ++i)
					hello.u8(uint8_t(me.rgb[i]));
				for (int i = 0; i < 5; ++i)
					hello.u8(uint8_t(me.weapons[i]));
				conn->send(net::MsgHello, hello);
				phase = Waiting;
				phaseStart = now;
				status = "CONNECTED - WAITING FOR HOST TO START...";
			}
			else if (r < 0 || now - phaseStart > 5000)
			{
				conn->close();
				phase = Searching;
				status = "COULD NOT CONNECT TO " + target;
			}
		}
		else if (phase == Waiting)
		{
			bool alive = conn->pump();
			net::MsgType type;
			net::Buffer msg;
			if (conn->nextMessage(type, msg))
			{
				if (type == net::MsgReject)
				{
					gfx.infoBox("HOST SAYS: " + msg.getStr());
					return nullptr;
				}
				if (type == net::MsgStart)
				{
					uint32_t seed = msg.getU32();
					int delay = msg.getU8();
					std::string blob = msg.getStr();

					std::string path = tempGamePath();
					{
						std::ofstream out(path, std::ios::binary);
						out.write(blob.data(), blob.size());
					}

					std::unique_ptr<Game> game;
					try
					{
						game = loadGame(path);
					}
					catch (std::exception& e)
					{
						std::remove(path.c_str());
						gfx.infoBox(std::string("COULD NOT LOAD GAME: ") + e.what());
						return nullptr;
					}
					std::remove(path.c_str());

					return std::unique_ptr<Controller>(
						new NetController(std::move(game), std::move(conn), false, seed, delay));
				}
			}
			else if (!alive || now - phaseStart > 15000)
			{
				gfx.infoBox("THE HOST DID NOT START THE GAME");
				return nullptr;
			}
		}

		std::vector<std::string> lines = {"JOIN NETWORK GAME", ""};
		if (phase == Searching)
			lines.push_back("SEARCHING YOUR NETWORK FOR A HOST...");
		lines.push_back(status);
		lines.push_back("");
		lines.push_back("YOU ARE THE LEFT WORM");
		lines.push_back("");
		if (phase == Searching)
			lines.push_back("I: TYPE THE HOST'S IP ADDRESS");
		lines.push_back("ESC: CANCEL");
		drawSetupScreen(lines);

		if (gfx.testSDLKeyOnce(SDL_SCANCODE_ESCAPE))
			return nullptr;

		if (phase == Searching && gfx.testSDLKeyOnce(SDL_SCANCODE_I))
		{
			drawSetupScreen({"JOIN NETWORK GAME", "", "TYPE THE IP ADDRESS SHOWN ON THE HOST'S SCREEN", "", "", "", "ENTER: CONNECT   ESC: BACK"});
			gfx.frozenScreen.copy(gfx.playRenderer.bmp);
			std::string ip = lastJoinIp;
			if (gfx.inputString(ip, 15, 160, 100, filterIp) && !ip.empty())
			{
				lastJoinIp = ip;
				connectTo(ip, ip);
			}
			gfx.clearKeys();
		}
	}
}

}

std::unique_ptr<Controller> runNetworkSetup(bool host)
{
	std::unique_ptr<Controller> result = host ? hostGame() : joinGame();
	gfx.clearKeys();
	return result;
}
