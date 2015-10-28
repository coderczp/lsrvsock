#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <lua.h>
#include <lauxlib.h>

/*
gcc --shared -fPIC -O2 -o lsrvsock.so lsrvsock.c
*/

#if LUA_VERSION_NUM < 502
#  define luaL_newlib(L,l) (lua_newtable(L), luaL_register(L,NULL,l))
#endif

#define META_NAME "lsrvsock{host}"
#define BUFFER_SIZE 1024

#ifdef WIN32
#  include <windows.h>
#  include <winsock2.h>

static void startup()
{
	WORD wVersionRequested;
	WSADATA wsaData;
	int err;

	wVersionRequested = MAKEWORD(2, 2);

	err = WSAStartup(wVersionRequested, &wsaData);
	if (err != 0) {
		printf("WSAStartup failed with error: %d\n", err);
		exit(1);
	}
}

static void sleep_ms(int ms)
{
	Sleep(ms);
}

#define EINTR WSAEINTR
#define EWOULDBLOCK WSAEWOULDBLOCK

#else

#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#define closesocket close

static void startup()
{
}

static void sleep_ms(int ms)
{
	usleep((useconds_t)ms * 1000);
}

#endif

struct socket {
	int listen_fd;
	int peer_fd;
	int closed;
};

static int fdcanread(int fd)
{
	struct timeval tv = { 0, 0 };
	fd_set rfds;
	int r = 0;

	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);

	r = select(fd + 1, &rfds, NULL, NULL, &tv);
	return r == 1;
}

static int test(struct socket *s, const char *welcome, size_t sz)
{
	if (s->closed) {
		closesocket(s->peer_fd);
		s->peer_fd = -1;
		s->closed = 0;
	}
	if (s->peer_fd < 0) {
		if (fdcanread(s->listen_fd)) {
			s->peer_fd = accept(s->listen_fd, NULL, NULL);
			if (s->peer_fd < 0) {
				return -1;
			}
			send(s->peer_fd, welcome, sz, 0);
		}
		if (s->peer_fd < 0) {
			return -1;
		}
	}
	if (fdcanread(s->peer_fd)) {
		return s->peer_fd;
	}
	return -1;

}

static int lua__new(lua_State *L)
{
	const char *addr = luaL_checkstring(L, 1);
	int port = luaL_checkinteger(L, 2);

	struct socket *s = lua_newuserdata(L, sizeof(*s));

	s->listen_fd = -1;
	s->peer_fd = -1;
	s->closed = 0;

	luaL_getmetatable(L, META_NAME);
	lua_setmetatable(L, -2);

	int lfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	int reuse = 1;
	setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, (void *)&reuse,
		   sizeof(int));

	struct sockaddr_in service;

	service.sin_family = AF_INET;
	service.sin_addr.s_addr = inet_addr(addr);
	service.sin_port = htons(port);

	if (bind(lfd, (const struct sockaddr *)&service, sizeof(service)) < 0) {
		closesocket(lfd);
		lua_pushnil(L);
		lua_pushfstring(L, "bind %s:%d failed", addr, port);
		return 2;
	}
	if (listen(lfd, 1) < 0) {
		lua_pushnil(L);
		lua_pushfstring(L, "listen failed");
		return 2;
	}
	s->listen_fd = lfd;

	return 1;
}

static int lua__sleep(lua_State *L)
{
	int ms = luaL_optinteger(L, 1, 0);
	sleep_ms(ms);
	return 0;
}

static int lua__read(lua_State *L)
{
	struct socket *s = lua_touserdata(L, 1);
	if (s == NULL || s->listen_fd < 0) {
		return luaL_error(L, "socket not found");
	}
	size_t sz = 0;
	const char *welcome = luaL_checklstring(L, 2, &sz);
	int peer_fd = test(s, welcome, sz);
	if (peer_fd >= 0) {
		char buffer[BUFFER_SIZE];
		int rd = recv(peer_fd, buffer, BUFFER_SIZE, 0);
		if (rd <= 0) {
			s->closed = 1;
			lua_pushboolean(L, 0);
			return 1;
		}
		lua_pushlstring(L, buffer, rd);
		return 1;
	}
	return 0;
}

static int lua__getpeername(lua_State *L)
{
       	socklen_t addrlen;
	struct sockaddr_in sa;
	struct socket *s = lua_touserdata(L, 1);
	if (s == NULL || s->listen_fd < 0 || s->peer_fd < 0) {
		return luaL_error(L, "start socket first");
	}
	if (getpeername(s->peer_fd, (struct sockaddr *)&sa, &addrlen)) {
		fprintf(stderr, "failed to getpeername\n");
		return 0;
	}
	const char *host = inet_ntoa(sa.sin_addr);
	lua_pushstring(L, host);
	lua_pushinteger(L, ntohs(sa.sin_port));
	return 2;
}


static int lua__write(lua_State *L)
{
	struct socket *s = lua_touserdata(L, 1);
	if (s == NULL || s->listen_fd < 0 || s->peer_fd < 0) {
		return luaL_error(L, "start socket first");
	}
	size_t sz = 0;
	const char *buffer = luaL_checklstring(L, 2, &sz);
	int p = 0;
	for (;;) {
		int wt = send(s->peer_fd, buffer + p, sz - p, 0);
		if (wt < 0) {
			switch (errno) {
			case EWOULDBLOCK:
			case EINTR:
				continue;
			default:
				closesocket(s->peer_fd);
				s->peer_fd = -1;
				lua_pushboolean(L, 0);
				return 1;
			}
		}
		if (wt == sz - p)
			break;
		p += wt;
	}
	if (s->closed) {
		closesocket(s->peer_fd);
		s->peer_fd = -1;
		s->closed = 0;
	}

	return 0;
}

static int lua__isconnected(lua_State *L)
{
	struct socket *s = lua_touserdata(L, 1);
	size_t sz;
	const char *welcome = luaL_checklstring(L, 2, &sz);
	int peer_fd = test(s, welcome, sz);
	if (s == NULL || s->listen_fd < 0 || peer_fd < 0)
		lua_pushboolean(L, 0);
	else
		lua_pushboolean(L, 1);
	return 1;
}

static int lua__gc(lua_State *L)
{
	struct socket *s = lua_touserdata(L, 1);
	if (s->listen_fd > 0) {
		closesocket(s->listen_fd);
		s->listen_fd = -1;
	}
	return 0;
}

static int opencls__lsrvsock(lua_State *L)
{
	luaL_Reg lmethods[] = {
		{"isconnected", lua__isconnected},
		{"read", lua__read},
		{"write", lua__write},
		{"getpeername", lua__getpeername},
		{NULL, NULL},
	};
	luaL_newmetatable(L, META_NAME);
	lua_newtable(L);
	luaL_register(L, NULL, lmethods);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction (L, lua__gc);
	lua_setfield (L, -2, "__gc");
	return 1;
}


int luaopen_lsrvsock(lua_State* L)
{
	luaL_Reg lfuncs[] = {
		{"new", lua__new},
		{"sleep", lua__sleep},
		{NULL, NULL},
	};
	opencls__lsrvsock(L);
	startup();
	luaL_newlib(L, lfuncs);
	return 1;
}

