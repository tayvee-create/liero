#ifndef LIERO_NET_NET_HPP
#define LIERO_NET_NET_HPP

#include <cstdint>
#include <string>
#include <vector>

// Minimal non-blocking TCP + UDP-broadcast helpers for two player network games.
// All sockets are non-blocking and are polled once per frame from the game loop.

namespace net
{

int const gamePort = 24850;      // TCP, game traffic
int const discoveryPort = 24851; // UDP, LAN discovery
uint16_t const protocolVersion = 1;
uint32_t const maxMessageSize = 16 * 1024 * 1024; // The initial game is well under 1 MB

enum MsgType : uint8_t
{
	MsgHello = 1,   // client -> host: version, tc name, player name, colour, weapons
	MsgStart = 2,   // host -> client: sync seed, input delay, serialized initial game
	MsgInput = 3,   // both: frame number + control events for that frame
	MsgQuit = 4,    // both: player left the match
	MsgCheck = 5,   // both: frame number + game checksum, for desync detection
	MsgReject = 6   // host -> client: reason string
};

struct Buffer
{
	std::vector<uint8_t> data;
	std::size_t pos = 0;

	void u8(uint8_t v) { data.push_back(v); }
	void u16(uint16_t v) { u8(v >> 8); u8(v & 0xff); }
	void u32(uint32_t v) { u16(v >> 16); u16(v & 0xffff); }
	void str(std::string const& s) { u32((uint32_t)s.size()); bytes(s.data(), s.size()); }
	void bytes(void const* p, std::size_t n)
	{
		auto b = static_cast<uint8_t const*>(p);
		data.insert(data.end(), b, b + n);
	}

	bool ok(std::size_t n) const { return pos + n <= data.size(); }
	uint8_t getU8() { return ok(1) ? data[pos++] : 0; }
	uint16_t getU16() { uint16_t h = getU8(); return uint16_t((h << 8) | getU8()); }
	uint32_t getU32() { uint32_t h = getU16(); return (h << 16) | getU16(); }
	std::string getStr()
	{
		uint32_t n = getU32();
		if (!ok(n)) { pos = data.size(); return std::string(); }
		std::string s(data.begin() + pos, data.begin() + pos + n);
		pos += n;
		return s;
	}
};

struct Connection
{
	Connection() {}
	explicit Connection(int fd);
	~Connection();
	Connection(Connection const&) = delete;
	Connection& operator=(Connection const&) = delete;

	// Starts a non-blocking connect. Returns false on immediate failure.
	bool connectTo(std::string const& ip, int port);
	// While connecting, returns 1 when connected, 0 while pending, -1 on failure.
	int pollConnect();

	void send(MsgType type, Buffer const& payload);
	// Sends queued data and receives pending data. Returns false if the connection is gone.
	bool pump();
	bool nextMessage(MsgType& type, Buffer& payload);

	bool isOpen() const { return fd >= 0; }
	void close();

	int fd = -1;
	bool connecting = false;
	std::vector<uint8_t> inbuf, outbuf;
};

struct Host
{
	~Host();
	bool start(std::string& error);
	// Returns an accepted socket or -1.
	int accept();
	// Replies to any discovery broadcasts with our name.
	void answerDiscovery(std::string const& name);
	void stop();

	int tcpFd = -1;
	int udpFd = -1;
};

struct FoundHost
{
	std::string ip;
	std::string name;
};

struct Discovery
{
	~Discovery();
	bool start();
	void broadcast();
	bool poll(FoundHost& found);

	int fd = -1;
};

// The primary IPv4 address of this Mac on the local network, or "" if none.
std::string localAddress();

}

#endif // LIERO_NET_NET_HPP
