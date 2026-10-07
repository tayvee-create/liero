#include "net.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace net
{

static char const discoveryQuery[] = "LIERO-NET?";
static char const discoveryReply[] = "LIERO-NET!";

static void setNonBlocking(int fd)
{
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static void setNoDelay(int fd)
{
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
}

Connection::Connection(int fd)
: fd(fd)
{
	setNonBlocking(fd);
	setNoDelay(fd);
}

Connection::~Connection()
{
	close();
}

void Connection::close()
{
	if (fd >= 0)
		::close(fd);
	fd = -1;
	connecting = false;
}

bool Connection::connectTo(std::string const& ip, int port)
{
	close();
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1)
		return false;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return false;
	setNonBlocking(fd);
	setNoDelay(fd);

	if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0)
		return true;
	if (errno != EINPROGRESS)
	{
		close();
		return false;
	}
	connecting = true;
	return true;
}

int Connection::pollConnect()
{
	if (fd < 0)
		return -1;
	if (!connecting)
		return 1;

	fd_set wfds;
	FD_ZERO(&wfds);
	FD_SET(fd, &wfds);
	timeval tv = {0, 0};
	if (select(fd + 1, 0, &wfds, 0, &tv) <= 0)
		return 0;

	int err = 0;
	socklen_t len = sizeof(err);
	getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
	if (err != 0)
	{
		close();
		return -1;
	}
	connecting = false;
	return 1;
}

void Connection::send(MsgType type, Buffer const& payload)
{
	Buffer header;
	header.u8(type);
	header.u32((uint32_t)payload.data.size());
	outbuf.insert(outbuf.end(), header.data.begin(), header.data.end());
	outbuf.insert(outbuf.end(), payload.data.begin(), payload.data.end());
	pump();
}

bool Connection::pump()
{
	if (fd < 0)
		return false;
	if (connecting)
		return true;

	while (!outbuf.empty())
	{
		ssize_t n = ::send(fd, outbuf.data(), outbuf.size(), 0);
		if (n > 0)
			outbuf.erase(outbuf.begin(), outbuf.begin() + n);
		else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
			break;
		else
		{
			close();
			return false;
		}
	}

	uint8_t buf[16384];
	while (true)
	{
		ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
		if (n > 0)
			inbuf.insert(inbuf.end(), buf, buf + n);
		else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
			break;
		else
		{
			// Keep already received data readable, but report the disconnect
			close();
			return false;
		}
	}
	return true;
}

bool Connection::nextMessage(MsgType& type, Buffer& payload)
{
	if (inbuf.size() < 5)
		return false;
	uint32_t len = (uint32_t(inbuf[1]) << 24) | (uint32_t(inbuf[2]) << 16) | (uint32_t(inbuf[3]) << 8) | inbuf[4];
	if (len > maxMessageSize)
	{
		// Not a Liero peer (or a hostile one); don't buffer gigabytes waiting for it
		close();
		inbuf.clear();
		return false;
	}
	if (inbuf.size() < 5 + len)
		return false;
	type = MsgType(inbuf[0]);
	payload.data.assign(inbuf.begin() + 5, inbuf.begin() + 5 + len);
	payload.pos = 0;
	inbuf.erase(inbuf.begin(), inbuf.begin() + 5 + len);
	return true;
}

Host::~Host()
{
	stop();
}

void Host::stop()
{
	if (tcpFd >= 0) ::close(tcpFd);
	if (udpFd >= 0) ::close(udpFd);
	tcpFd = udpFd = -1;
}

bool Host::start(std::string& error)
{
	stop();
	int one = 1;

	tcpFd = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(tcpFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(gamePort);
	if (bind(tcpFd, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(tcpFd, 1) != 0)
	{
		error = std::string("CANNOT OPEN PORT ") + std::to_string(gamePort) + ": " + std::strerror(errno);
		stop();
		return false;
	}
	setNonBlocking(tcpFd);

	// Discovery is optional; joining by IP still works if this fails
	udpFd = socket(AF_INET, SOCK_DGRAM, 0);
	setsockopt(udpFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	setsockopt(udpFd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
	addr.sin_port = htons(discoveryPort);
	if (bind(udpFd, (sockaddr*)&addr, sizeof(addr)) != 0)
	{
		::close(udpFd);
		udpFd = -1;
	}
	else
		setNonBlocking(udpFd);
	return true;
}

int Host::accept()
{
	if (tcpFd < 0)
		return -1;
	return ::accept(tcpFd, 0, 0);
}

void Host::answerDiscovery(std::string const& name)
{
	if (udpFd < 0)
		return;
	char buf[64];
	sockaddr_in from;
	socklen_t fromLen = sizeof(from);
	ssize_t n;
	while ((n = recvfrom(udpFd, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen)) > 0)
	{
		if (std::size_t(n) == sizeof(discoveryQuery) - 1
		 && std::memcmp(buf, discoveryQuery, n) == 0)
		{
			std::string reply = discoveryReply + name;
			sendto(udpFd, reply.data(), reply.size(), 0, (sockaddr*)&from, fromLen);
		}
		fromLen = sizeof(from);
	}
}

Discovery::~Discovery()
{
	if (fd >= 0)
		::close(fd);
}

bool Discovery::start()
{
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return false;
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
	setNonBlocking(fd);
	return true;
}

void Discovery::broadcast()
{
	if (fd < 0)
		return;

	auto sendTo = [&](in_addr a) {
		sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(discoveryPort);
		addr.sin_addr = a;
		sendto(fd, discoveryQuery, sizeof(discoveryQuery) - 1, 0, (sockaddr*)&addr, sizeof(addr));
	};

	in_addr all;
	all.s_addr = htonl(INADDR_BROADCAST);
	sendTo(all);

	// The limited broadcast address only goes out of one interface on macOS,
	// so also send to each interface's subnet broadcast address.
	ifaddrs* ifs = 0;
	if (getifaddrs(&ifs) == 0)
	{
		for (ifaddrs* i = ifs; i; i = i->ifa_next)
		{
			if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET
			 && (i->ifa_flags & IFF_BROADCAST) && (i->ifa_flags & IFF_UP)
			 && i->ifa_broadaddr)
			{
				sendTo(((sockaddr_in*)i->ifa_broadaddr)->sin_addr);
			}
		}
		freeifaddrs(ifs);
	}
}

bool Discovery::poll(FoundHost& found)
{
	if (fd < 0)
		return false;
	char buf[128];
	sockaddr_in from;
	socklen_t fromLen = sizeof(from);
	ssize_t n;
	std::size_t const prefix = sizeof(discoveryReply) - 1;
	while ((n = recvfrom(fd, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen)) > 0)
	{
		if (std::size_t(n) >= prefix && std::memcmp(buf, discoveryReply, prefix) == 0)
		{
			char ip[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
			found.ip = ip;
			found.name.assign(buf + prefix, buf + n);
			return true;
		}
		fromLen = sizeof(from);
	}
	return false;
}

std::string localAddress()
{
	std::string best;
	ifaddrs* ifs = 0;
	if (getifaddrs(&ifs) != 0)
		return best;
	for (ifaddrs* i = ifs; i; i = i->ifa_next)
	{
		if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET)
			continue;
		if ((i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP))
			continue;
		char ip[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &((sockaddr_in*)i->ifa_addr)->sin_addr, ip, sizeof(ip));
		// Prefer en0 (built-in Wi-Fi/Ethernet), but take anything otherwise
		if (best.empty() || std::strcmp(i->ifa_name, "en0") == 0)
			best = ip;
	}
	freeifaddrs(ifs);
	return best;
}

}
