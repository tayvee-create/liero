#ifndef LIERO_CONTROLLER_NET_CONTROLLER_HPP
#define LIERO_CONTROLLER_NET_CONTROLLER_HPP

#include "commonController.hpp"

#include "../game.hpp"
#include "../rand.hpp"
#include "../net/net.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

struct WeaponSelection;
struct ReplayWriter;

// Two player game over the network using deterministic lockstep: both machines
// start from the same serialized game and only exchange control events. Frame N
// is simulated once both players' events for frame N have arrived; local events
// are scheduled `inputDelay` frames ahead to hide network latency.
//
// The host always controls worm 0 (left), the joining player worm 1 (right).
struct NetController : CommonController
{
	NetController(std::unique_ptr<Game> game, std::unique_ptr<net::Connection> conn,
		bool isHost, uint32_t syncSeed, int inputDelay);
	~NetController();

	void onKey(int key, bool keyState);
	void unfocus();
	void focus();
	bool process();
	void draw(Renderer& renderer, bool useSpectatorViewports);
	bool running();
	Level* currentLevel();
	Game* currentGame();
	void swapLevel(Level& newLevel);

	// Tells the opponent we are leaving (used when the app quits mid-match)
	void notifyQuit();

private:
	void changeState(GameState newState);
	void pumpNetwork();
	void sendLocalInput();
	void stepFrame();
	void applyEvent(Worm& worm, uint8_t ev);
	void botInput();
	void abort(std::string const& reason);
	void startRecording();
	void endRecord();

	std::unique_ptr<Game> game;
	std::unique_ptr<WeaponSelection> ws;
	std::unique_ptr<ReplayWriter> replay;
	std::unique_ptr<net::Connection> conn;
	GameState state;
	int fadeValue;
	bool goingToMenu;

	int localIdx, remoteIdx;
	int inputDelay;
	Rand syncRand; // Stands in for gfx.rand during weapon selection, which uses it

	uint32_t simFrame, nextLocalFrame;
	std::map<uint32_t, std::vector<uint8_t>> inputs[2];
	std::vector<uint8_t> pendingEvents;
	std::map<uint32_t, uint32_t> localChecks, remoteChecks;

	bool desynced;
	bool aborted;
	std::string abortReason;
	int stalledTicks;
	int silentTicks;
	int escapeTimer;
};

// Command line shortcuts used for testing: --net-host, --net-join <ip>, --net-bot, --one-player easy|hard
struct NetAutoStart
{
	int mode = 0; // 1 = host, 2 = join, 3 = one player easy, 4 = one player hard
	std::string ip;
	bool bot = false; // Local worm plays random inputs and logs checksums to stderr
};
extern NetAutoStart netAutoStart;

// Called on app quit: notifies the opponent if a network match is in progress.
void netShutdown();

// Shows the host/join screens and returns a ready controller, or null if cancelled.
std::unique_ptr<Controller> runNetworkSetup(bool host);

#endif // LIERO_CONTROLLER_NET_CONTROLLER_HPP
