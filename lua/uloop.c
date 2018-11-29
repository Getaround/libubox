/*
 * Copyright (C) 2012 John Crispin <blogic@openwrt.org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*** Binding for libubox into lua
 * @module uloop
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include "../uloop.h"
#include "../list.h"

struct lua_uloop_fd {
	struct uloop_fd fd;
	int r;
	int fd_r;
};

struct lua_uloop_timeout {
	struct uloop_timeout t;
	int r;
};

struct lua_uloop_process {
	struct uloop_process p;
	int r;
};

static lua_State *state;

static __attribute__((unused)) void stackDump (lua_State *L) {
	int i;
	int top = lua_gettop(L);  /* depth of the stack */
	for (i = 1; i <= top; i++) {  /* repeat for each level */
		int t = lua_type(L, i);
		printf("%d: ", i);
		switch (t) {
			case LUA_TSTRING: {  /* strings */
								  printf("'%s'", lua_tostring(L, i));
								  break;
							  }
			case LUA_TBOOLEAN: {  /* Booleans */
								   printf(lua_toboolean(L, i) ? "true" : "false");
								   break; }
			case LUA_TNUMBER: {  /* numbers */
								  printf("%g", lua_tonumber(L, i));
								  break;
							  }
			default: {  /* other values */
						 printf("%s", lua_typename(L, t));
						 break; }
		}
		printf(", ");  /* put a separator */
	}
	printf("\n");  /* end the listing */
}

static void *
ul_create_userdata(lua_State *L, size_t size, const luaL_Reg *reg, lua_CFunction gc)
{

	void *ret = lua_newuserdata(L, size); // S: nud

	memset(ret, 0, size);
	lua_createtable(L, 0, 2);   // create a new table, with hint 2 other elements, S: nud, table
	lua_pushvalue(L, -1);       // duplicate it, now S: nud, table, table
	lua_setfield(L, -2, "__index"); // set t[__index] = t.  S: nud, table
	lua_pushcfunction(L, gc);   // nud, table, gc
	lua_setfield(L, -2, "__gc"); // add t[__gc] = gc to t, S: nud, table
	lua_pushvalue(L, -1);      // nud, table, table
	lua_setmetatable(L, -3);   // set on metatable for nud, s: nud, table
	lua_pushvalue(L, -2);      // copy nud, s: nud, table, nud
	luaL_setfuncs(L, reg, 1);   // registers reg, with 1 up func. s: nud, table
	lua_pushvalue(L, -2);      // duplicate nud, s: nud, table, nud

	return ret;
}

static void ul_timer_cb(struct uloop_timeout *t)
{
	struct lua_uloop_timeout *tout = container_of(t, struct lua_uloop_timeout, t);

	lua_getglobal(state, "__uloop_cb");
	lua_rawgeti(state, -1, tout->r);
	lua_remove(state, -2);

	lua_call(state, 0, 0);

}

static int ul_timer_set(lua_State *L)
{
	struct lua_uloop_timeout *tout;
	lua_Integer set;
	int isint;

	if (!lua_isnumber(L, -1)) {
		lua_pushstring(L, "invalid arg list");
		lua_error(L);

		return 0;
	}

	set = lua_tointegerx(L, -1, &isint);
	if (!isint) {
		lua_Number n = lua_tonumber(L, -1);
		set = n;
	}
	tout = lua_touserdata(L, 1);
	uloop_timeout_set(&tout->t, set);

	return 1;
}

static int ul_timer_remaining(lua_State *L)
{
	struct lua_uloop_timeout *tout;

	tout = lua_touserdata(L, 1);
	lua_pushnumber(L, uloop_timeout_remaining(&tout->t));
	return 1;
}

static int ul_timer_free(lua_State *L)
{
	struct lua_uloop_timeout *tout = lua_touserdata(L, 1);

	uloop_timeout_cancel(&tout->t);

	/* obj.__index.__gc = nil , make sure executing only once*/
	lua_getfield(L, -1, "__index");
	lua_pushstring(L, "__gc");
	lua_pushnil(L);
	lua_settable(L, -3);

	lua_getglobal(state, "__uloop_cb");
	luaL_unref(state, -1, tout->r);

	return 1;
}

static const luaL_Reg timer_m[] = {
	{ "set", ul_timer_set },
	{ "remaining", ul_timer_remaining },
	{ "cancel", ul_timer_free },
	{ NULL, NULL }
};

static int ul_timer(lua_State *L)
{
	struct lua_uloop_timeout *tout;
	lua_Integer set = 0;
	int ref;

	if (lua_isnumber(L, -1)) {
		set = lua_tointeger(L, -1);
		lua_pop(L, 1);
	}

	if (!lua_isfunction(L, -1)) {
		lua_pushstring(L, "invalid arg list");
		lua_error(L);

		return 0;
	}

	lua_getglobal(L, "__uloop_cb");
	lua_pushvalue(L, -2);
	ref = luaL_ref(L, -2);

	tout = ul_create_userdata(L, sizeof(*tout), timer_m, ul_timer_free);
	tout->r = ref;
	tout->t.cb = ul_timer_cb;

	if (set)
		uloop_timeout_set(&tout->t, set);

	return 1;
}

static void ul_ufd_cb(struct uloop_fd *fd, unsigned int events)
{
	struct lua_uloop_fd *ufd = container_of(fd, struct lua_uloop_fd, fd);

	lua_getglobal(state, "__uloop_cb");
	lua_rawgeti(state, -1, ufd->r);
	lua_remove(state, -2);

	/* push fd object */
	lua_getglobal(state, "__uloop_fds");
	lua_rawgeti(state, -1, ufd->fd_r);
	lua_remove(state, -2);

	/* push events */
	lua_pushinteger(state, events);
	lua_call(state, 2, 0);
}


static int get_sock_fd(lua_State* L, int idx) {
	int fd;
	if(lua_isnumber(L, idx)) {
		fd = lua_tonumber(L, idx);
	} else {
		luaL_checktype(L, idx, LUA_TUSERDATA);
		lua_getfield(L, idx, "getfd");
		if(lua_isnil(L, -1))
			return luaL_error(L, "socket type missing 'getfd' method");
		lua_pushvalue(L, idx - 1);
		lua_call(L, 1, 1);
		fd = lua_tointeger(L, -1);
		lua_pop(L, 1);
	}
	return fd;
}

static int ul_ufd_delete(lua_State *L)
{
	struct lua_uloop_fd *ufd = lua_touserdata(L, 1);

	uloop_fd_delete(&ufd->fd);

	/* obj.__index.__gc = nil , make sure executing only once*/
	lua_getfield(L, -1, "__index");
	lua_pushstring(L, "__gc");
	lua_pushnil(L);
	lua_settable(L, -3);

	lua_getglobal(state, "__uloop_cb");
	luaL_unref(state, -1, ufd->r);
	lua_remove(state, -1);

	lua_getglobal(state, "__uloop_fds");
	luaL_unref(state, -1, ufd->fd_r);
	lua_remove(state, -1);

	return 1;
}

static const luaL_Reg ufd_m[] = {
	{ "delete", ul_ufd_delete },
	{ NULL, NULL }
};

/***
 * Add a file descriptor to uloop for call on activity
 * @function fd_add
 * @tparam int fd File Descriptor to add
 * @tparam function func Callback function
 * @tparam int Option flags or of: uloop.ULOOP_READ, 
 *             uloop.ULOOP_WRITE, uloop.EDGE_TRIGGER, uloop.ULOOP_BLOCKING
 */
static int ul_ufd_add(lua_State *L)
{
	struct lua_uloop_fd *ufd;
	int fd = 0;
	unsigned int flags = 0;
	int ref;
	int fd_ref;

	if (lua_isnumber(L, -1)) {
		flags = lua_tointeger(L, -1);
		lua_pop(L, 1);
	}

	if (!lua_isfunction(L, -1)) {
		lua_pushstring(L, "invalid arg list");
		lua_error(L);

		return 0;
	}

	fd = get_sock_fd(L, -2);

	lua_getglobal(L, "__uloop_cb");
	lua_pushvalue(L, -2);
	ref = luaL_ref(L, -2);
	lua_pop(L, 1);

	lua_getglobal(L, "__uloop_fds");
	lua_pushvalue(L, -3);
	fd_ref = luaL_ref(L, -2);
	lua_pop(L, 1);

	ufd = ul_create_userdata(L, sizeof(*ufd), ufd_m, ul_ufd_delete);
	ufd->r = ref;
	ufd->fd.fd = fd;
	ufd->fd_r = fd_ref;
	ufd->fd.cb = ul_ufd_cb;
	if (flags)
		uloop_fd_add(&ufd->fd, flags);

	return 1;
}

static int ul_process_free(lua_State *L)
{
	struct lua_uloop_process *proc = lua_touserdata(L, 1);

	/* obj.__index.__gc = nil , make sure executing only once*/
	lua_getfield(L, -1, "__index");
	lua_pushstring(L, "__gc");
	lua_pushnil(L);
	lua_settable(L, -3);

	if (proc->r != LUA_NOREF) {
		uloop_process_delete(&proc->p);

		lua_getglobal(state, "__uloop_cb");
		luaL_unref(state, -1, proc->r);
		lua_remove(state, -1);
	}

	return 1;
}

static int ul_process_pid(lua_State *L)
{
	struct lua_uloop_process *proc = lua_touserdata(L, 1);

	if (proc->p.pid) {
		lua_pushnumber(L, proc->p.pid);
		return 1;
	}

	return 0;
}

static const luaL_Reg process_m[] = {
	{ "delete", ul_process_free },
	{ "pid", ul_process_pid },
	{ NULL, NULL }
};

static void ul_process_cb(struct uloop_process *p, int ret)
{
	struct lua_uloop_process *proc = container_of(p, struct lua_uloop_process, p);

	lua_getglobal(state, "__uloop_cb");
	lua_rawgeti(state, -1, proc->r);

	luaL_unref(state, -2, proc->r);
	proc->r = LUA_NOREF;
	lua_remove(state, -2);
	lua_pushinteger(state, ret >> 8);
	lua_call(state, 1, 0);
}

/***
 * Create a new process and get notified of its termination
 * @function process
 * @tparam string process to execute
 * @tparam table A table of arguments to pass to the process
 * @tparam table A table of environment variables to set
 * @tparam function Callback when the process exits
 */
static int ul_process(lua_State *L)
{
	struct lua_uloop_process *proc;
	pid_t pid;
	int ref;

	if (!lua_isfunction(L, -1) || !lua_istable(L, -2) ||
			!lua_istable(L, -3) || !lua_isstring(L, -4)) {
		lua_pushstring(L, "invalid arg list");
		lua_error(L);

		return 0;
	}

	pid = fork();

	if (pid == -1) {
		lua_pushstring(L, "failed to fork");
		lua_error(L);

		return 0;
	}

	if (pid == 0) {
		/* child */
		int argn = lua_rawlen(L, -3);
		int envn = lua_rawlen(L, -2);
		char** argp = malloc(sizeof(char*) * (argn + 2));
		char** envp = malloc(sizeof(char*) * (envn + 1));
		int i = 1;

		if (!argp || !envp)
			_exit(-1);

		argp[0] = (char*) lua_tostring(L, -4);
		for (i = 1; i <= argn; i++) {
			lua_rawgeti(L, -3, i);
			argp[i] = (char*) lua_tostring(L, -1);
			lua_pop(L, 1);
		}
		argp[i] = NULL;

		for (i = 1; i <= envn; i++) {
			lua_rawgeti(L, -2, i);
			envp[i - 1] = (char*) lua_tostring(L, -1);
			lua_pop(L, 1);
		}
		envp[i - 1] = NULL;

		execve(*argp, argp, envp);
		_exit(-1);
	}

	lua_getglobal(L, "__uloop_cb");
	lua_pushvalue(L, -2);
	ref = luaL_ref(L, -2);

	proc = ul_create_userdata(L, sizeof(*proc), process_m, ul_process_free);
	proc->r = ref;
	proc->p.pid = pid;
	proc->p.cb = ul_process_cb;
	uloop_process_add(&proc->p);

	return 1;
}

/***
 * Create a new process and get notified of its termination
 * @function pid_add
 * @tparam int pid The process id
 * @tparam function Callback when the process exits
 */
static int ul_pid_add(lua_State *L)
{
	struct lua_uloop_process *proc;
	lua_Integer pid;
	int isint;
	int ref;

	pid = lua_tointegerx(L, 1, &isint);
	if (!isint) {
		lua_pushliteral(L, "First argument should be pid");
		lua_error(L);
	}

	if (!lua_isfunction(L, 2)) {
		lua_pushliteral(L, "Second argument should be callback function");
		lua_error(L);
	}

	lua_getglobal(L, "__uloop_cb");
	lua_pushvalue(L, -2);
	ref = luaL_ref(L, -2);

	proc = ul_create_userdata(L, sizeof(*proc), process_m, ul_process_free);
	proc->r = ref;
	proc->p.pid = pid;
	proc->p.cb = ul_process_cb;
	uloop_process_add(&proc->p);

	return 1;
}

static int ul_init(lua_State *L)
{
	uloop_init();
	lua_pushboolean(L, 1);

	return 1;
}

static int ul_run(lua_State *L)
{
	uloop_run();
	lua_pushboolean(L, 1);

	return 1;
}

static int ul_end(lua_State *L)
{
	uloop_end();
	return 1;
}

static luaL_Reg uloop_func[] = {
	{"init", ul_init},
	{"run", ul_run},
	{"timer", ul_timer},
	{"process", ul_process},
	{"pid_add", ul_pid_add},
	{"fd_add", ul_ufd_add},
	{"cancel", ul_end},
	{NULL, NULL},
};

/* avoid warnings about missing declarations */
int luaopen_uloop(lua_State *L);
int luaclose_uloop(lua_State *L);

int luaopen_uloop(lua_State *L)
{
	state = L;

	lua_createtable(L, 1, 0);
	lua_setglobal(L, "__uloop_cb");

	lua_createtable(L, 1, 0);
	lua_setglobal(L, "__uloop_fds");

    luaL_newlib(L, uloop_func);
	lua_pushstring(L, "_VERSION");
	lua_pushstring(L, "1.0");
	lua_rawset(L, -3);

	lua_pushstring(L, "ULOOP_READ");
	lua_pushinteger(L, ULOOP_READ);
	lua_rawset(L, -3);

	lua_pushstring(L, "ULOOP_WRITE");
	lua_pushinteger(L, ULOOP_WRITE);
	lua_rawset(L, -3);

	lua_pushstring(L, "ULOOP_EDGE_TRIGGER");
	lua_pushinteger(L, ULOOP_EDGE_TRIGGER);
	lua_rawset(L, -3);

	lua_pushstring(L, "ULOOP_BLOCKING");
	lua_pushinteger(L, ULOOP_BLOCKING);
	lua_rawset(L, -3);

	return 1;
}

int luaclose_uloop(lua_State *L)
{
	lua_pushstring(L, "Called");

	return 1;
}
