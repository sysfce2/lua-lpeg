
#include "lua.h"
#include "lauxlib.h"

#include "lpcap.h"
#include "lptypes.h"


#define captype(cap)	((cap)->kind)

#define isclosecap(cap)	(captype(cap) == Cclose)

#define closeaddr(cs,c)	((cs)->s + (c)->index + (c)->siz - 1)

#define isfullcap(cap)	((cap)->siz != 0)

#define getfromktable(cs,v)	lua_rawgeti((cs)->L, ktableidx((cs)->ptop), v)

#define pushluaval(cs)		getfromktable(cs, (cs)->cap->idx)



/*
** Put at the cache for Lua values the value indexed by 'v' in ktable
** of the running pattern (if it is not there yet); returns its index.
*/
static int updatecache (CapState *cs, int v) {
  int idx = cs->ptop + 1;  /* stack index of cache for Lua values */
  if (v != cs->valuecached) {  /* not there? */
    getfromktable(cs, v);  /* get value from 'ktable' */
    lua_replace(cs->L, idx);  /* put it at reserved stack position */
    cs->valuecached = v;  /* keep track of what is there */
  }
  return idx;
}


static int pushcapture (CapState *cs);


/*
** Goes back in a list of captures looking for an open capture
** corresponding to a close
*/
static Capture *findopen (Capture *cap) {
  int n = 0;  /* number of closes waiting an open */
  for (;;) {
    cap--;
    if (isclosecap(cap)) n++;  /* one more open to skip */
    else if (!isfullcap(cap))
      if (n-- == 0) return cap;
  }
}


/*
** Go to the next capture
*/
static void nextcap (CapState *cs) {
  Capture *cap = cs->cap;
  if (!isfullcap(cap)) {  /* not a single capture? */
    int n = 0;  /* number of opens waiting a close */
    for (;;) {  /* look for corresponding close */
      cap++;
      if (isclosecap(cap)) {
        if (n-- == 0) break;
      }
      else if (!isfullcap(cap)) n++;
    }
  }
  cs->cap = cap + 1;  /* + 1 to skip last close (or entire single capture) */
}


/*
** Push on the Lua stack all values generated by nested captures inside
** the current capture. Returns number of values pushed. 'addextra'
** makes it push the entire match after all captured values. The
** entire match is pushed also if there are no other nested values,
** so the function never returns zero.
*/
static int pushnestedvalues (CapState *cs, int addextra) {
  Capture *co = cs->cap;
  if (isfullcap(cs->cap++)) {  /* no nested captures? */
    /* push whole match */
    lua_pushlstring(cs->L, cs->s + co->index, co->siz - 1);
    return 1;  /* that is it */
  }
  else {
    int n = 0;
    while (!isclosecap(cs->cap))  /* repeat for all nested patterns */
      n += pushcapture(cs);
    if (addextra || n == 0) {  /* need extra? */
      /* push whole match */
      lua_pushlstring(cs->L, cs->s + co->index, cs->cap->index - co->index);
      n++;
    }
    cs->cap++;  /* skip close entry */
    return n;
  }
}


/*
** Push only the first value generated by nested captures
*/
static void pushonenestedvalue (CapState *cs) {
  int n = pushnestedvalues(cs, 0);
  if (n > 1)
    lua_pop(cs->L, n - 1);  /* pop extra values */
}


/*
** Try to find a named group capture with the name given at the top of
** the stack; goes backward from 'cap'.
*/
static Capture *findback (CapState *cs, Capture *cap) {
  lua_State *L = cs->L;
  while (cap-- > cs->ocap) {  /* repeat until end of list */
    if (isclosecap(cap))
      cap = findopen(cap);  /* skip nested captures */
    else if (!isfullcap(cap))
      continue; /* opening an enclosing capture: skip and get previous */
    if (captype(cap) == Cgroup) {
      getfromktable(cs, cap->idx);  /* get group name */
      if (lp_equal(L, -2, -1)) {  /* right group? */
        lua_pop(L, 2);  /* remove reference name and group name */
        return cap;
      }
      else lua_pop(L, 1);  /* remove group name */
    }
  }
  luaL_error(L, "back reference '%s' not found", lua_tostring(L, -1));
  return NULL;  /* to avoid warnings */
}


/*
** Back-reference capture. Return number of values pushed.
*/
static int backrefcap (CapState *cs) {
  int n;
  Capture *curr = cs->cap;
  pushluaval(cs);  /* reference name */
  cs->cap = findback(cs, curr);  /* find corresponding group */
  n = pushnestedvalues(cs, 0);  /* push group's values */
  cs->cap = curr + 1;
  return n;
}


/*
** Table capture: creates a new table and populates it with nested
** captures.
*/
static int tablecap (CapState *cs) {
  lua_State *L = cs->L;
  int n = 0;
  lua_newtable(L);
  if (isfullcap(cs->cap++))
    return 1;  /* table is empty */
  while (!isclosecap(cs->cap)) {
    if (captype(cs->cap) == Cgroup && cs->cap->idx != 0) {  /* named group? */
      pushluaval(cs);  /* push group name */
      pushonenestedvalue(cs);
      lua_settable(L, -3);
    }
    else {  /* not a named group */
      int i;
      int k = pushcapture(cs);
      for (i = k; i > 0; i--)  /* store all values into table */
        lua_rawseti(L, -(i + 1), n + i);
      n += k;
    }
  }
  cs->cap++;  /* skip close entry */
  return 1;  /* number of values pushed (only the table) */
}


/*
** Table-query capture
*/
static int querycap (CapState *cs) {
  int idx = cs->cap->idx;
  pushonenestedvalue(cs);  /* get nested capture */
  lua_gettable(cs->L, updatecache(cs, idx));  /* query cap. value at table */
  if (!lua_isnil(cs->L, -1))
    return 1;
  else {  /* no value */
    lua_pop(cs->L, 1);  /* remove nil */
    return 0;
  }
}


/*
** Fold capture
*/
static int foldcap (CapState *cs) {
  int n;
  lua_State *L = cs->L;
  int idx = cs->cap->idx;
  if (isfullcap(cs->cap++) ||  /* no nested captures? */
      isclosecap(cs->cap) ||  /* no nested captures (large subject)? */
      (n = pushcapture(cs)) == 0)  /* nested captures with no values? */
    return luaL_error(L, "no initial value for fold capture");
  if (n > 1)
    lua_pop(L, n - 1);  /* leave only one result for accumulator */
  while (!isclosecap(cs->cap)) {
    lua_pushvalue(L, updatecache(cs, idx));  /* get folding function */
    lua_insert(L, -2);  /* put it before accumulator */
    n = pushcapture(cs);  /* get next capture's values */
    lua_call(L, n + 1, 1);  /* call folding function */
  }
  cs->cap++;  /* skip close entry */
  return 1;  /* only accumulator left on the stack */
}


/*
** Function capture
*/
static int functioncap (CapState *cs) {
  int n;
  int top = lua_gettop(cs->L);
  pushluaval(cs);  /* push function */
  n = pushnestedvalues(cs, 0);  /* push nested captures */
  lua_call(cs->L, n, LUA_MULTRET);  /* call function */
  return lua_gettop(cs->L) - top;  /* return function's results */
}


/*
** Accumulator capture
*/
static int accumulatorcap (CapState *cs) {
  lua_State *L = cs->L;
  int n;
  if (lua_gettop(L) < cs->firstcap)
    luaL_error(L, "no previous value for accumulator capture");
  pushluaval(cs);  /* push function */
  lua_insert(L, -2);  /* previous value becomes first argument */
  n = pushnestedvalues(cs, 0);  /* push nested captures */
  lua_call(L, n + 1, 1);  /* call function */
  return 0;  /* did not add any extra value */
}


/*
** Select capture
*/
static int numcap (CapState *cs) {
  int idx = cs->cap->idx;  /* value to select */
  if (idx == 0) {  /* no values? */
    nextcap(cs);  /* skip entire capture */
    return 0;  /* no value produced */
  }
  else {
    int n = pushnestedvalues(cs, 0);
    if (n < idx)  /* invalid index? */
      return luaL_error(cs->L, "no capture '%d'", idx);
    else {
      lua_pushvalue(cs->L, -(n - idx + 1));  /* get selected capture */
      lua_replace(cs->L, -(n + 1));  /* put it in place of 1st capture */
      lua_pop(cs->L, n - 1);  /* remove other captures */
      return 1;
    }
  }
}


/*
** Return the stack index of the first runtime capture in the given
** list of captures (or zero if no runtime captures)
*/
int finddyncap (Capture *cap, Capture *last) {
  for (; cap < last; cap++) {
    if (cap->kind == Cruntime)
      return cap->idx;  /* stack position of first capture */
  }
  return 0;  /* no dynamic captures in this segment */
}


/*
** Calls a runtime capture. Returns number of captures "removed" by the
** call, that is, those inside the group capture. Captures to be added
** are on the Lua stack.
*/
int runtimecap (CapState *cs, Capture *close, const char *s, int *rem) {
  int n, id;
  lua_State *L = cs->L;
  int otop = lua_gettop(L);
  Capture *open = findopen(close);  /* get open group capture */
  assert(captype(open) == Cgroup);
  id = finddyncap(open, close);  /* get first dynamic capture argument */
  close->kind = Cclose;  /* closes the group */
  close->index = s - cs->s;
  cs->cap = open; cs->valuecached = 0;  /* prepare capture state */
  luaL_checkstack(L, 4, "too many runtime captures");
  pushluaval(cs);  /* push function to be called */
  lua_pushvalue(L, SUBJIDX);  /* push original subject */
  lua_pushinteger(L, s - cs->s + 1);  /* push current position */
  n = pushnestedvalues(cs, 0);  /* push nested captures */
  lua_call(L, n + 2, LUA_MULTRET);  /* call dynamic function */
  if (id > 0) {  /* are there old dynamic captures to be removed? */
    int i;
    for (i = id; i <= otop; i++)
      lua_remove(L, id);  /* remove old dynamic captures */
    *rem = otop - id + 1;  /* total number of dynamic captures removed */
  }
  else
    *rem = 0;  /* no dynamic captures removed */
  return close - open - 1;  /* number of captures to be removed */
}


/*
** Auxiliary structure for substitution and string captures: keep
** information about nested captures for future use, avoiding to push
** string results into Lua
*/
typedef struct StrAux {
  int isstring;  /* whether capture is a string */
  union {
    Capture *cp;  /* if not a string, respective capture */
    struct {  /* if it is a string... */
      const char *s;  /* ... starts here */
      const char *e;  /* ... ends here */
    } s;
  } u;
} StrAux;

#define MAXSTRCAPS	10

/*
** Collect values from current capture into array 'cps'. Current
** capture must be Cstring (first call) or Csimple (recursive calls).
** (In first call, fills %0 with whole match for Cstring.)
** Returns number of elements in the array that were filled.
*/
static int getstrcaps (CapState *cs, StrAux *cps, int n) {
  int k = n++;
  cps[k].isstring = 1;  /* get string value */
  cps[k].u.s.s = cs->s + cs->cap->index;  /* starts here */
  if (!isfullcap(cs->cap++)) {  /* nested captures? */
    while (!isclosecap(cs->cap)) {  /* traverse them */
      if (n >= MAXSTRCAPS)  /* too many captures? */
        nextcap(cs);  /* skip extra captures (will not need them) */
      else if (captype(cs->cap) == Csimple)  /* string? */
        n = getstrcaps(cs, cps, n);  /* put info. into array */
      else {
        cps[n].isstring = 0;  /* not a string */
        cps[n].u.cp = cs->cap;  /* keep original capture */
        nextcap(cs);
        n++;
      }
    }
    cs->cap++;  /* skip close */
  }
  cps[k].u.s.e = closeaddr(cs, cs->cap - 1);  /* ends here */
  return n;
}


/*
** add next capture value (which should be a string) to buffer 'b'
*/
static int addonestring (luaL_Buffer *b, CapState *cs, const char *what);


/*
** String capture: add result to buffer 'b' (instead of pushing
** it into the stack)
*/
static void stringcap (luaL_Buffer *b, CapState *cs) {
  StrAux cps[MAXSTRCAPS];
  int n;
  size_t len, i;
  const char *fmt;  /* format string */
  fmt = lua_tolstring(cs->L, updatecache(cs, cs->cap->idx), &len);
  n = getstrcaps(cs, cps, 0) - 1;  /* collect nested captures */
  for (i = 0; i < len; i++) {  /* traverse them */
    if (fmt[i] != '%')  /* not an escape? */
      luaL_addchar(b, fmt[i]);  /* add it to buffer */
    else if (fmt[++i] < '0' || fmt[i] > '9')  /* not followed by a digit? */
      luaL_addchar(b, fmt[i]);  /* add to buffer */
    else {
      int l = fmt[i] - '0';  /* capture index */
      if (l > n)
        luaL_error(cs->L, "invalid capture index (%d)", l);
      else if (cps[l].isstring)
        luaL_addlstring(b, cps[l].u.s.s, cps[l].u.s.e - cps[l].u.s.s);
      else {
        Capture *curr = cs->cap;
        cs->cap = cps[l].u.cp;  /* go back to evaluate that nested capture */
        if (!addonestring(b, cs, "capture"))
          luaL_error(cs->L, "no values in capture index %d", l);
        cs->cap = curr;  /* continue from where it stopped */
      }
    }
  }
}


/*
** Substitution capture: add result to buffer 'b'
*/
static void substcap (luaL_Buffer *b, CapState *cs) {
  const char *curr = cs->s + cs->cap->index;
  if (isfullcap(cs->cap))  /* no nested captures? */
    luaL_addlstring(b, curr, cs->cap->siz - 1);  /* keep original text */
  else {
    cs->cap++;  /* skip open entry */
    while (!isclosecap(cs->cap)) {  /* traverse nested captures */
      const char *next = cs->s + cs->cap->index;
      luaL_addlstring(b, curr, next - curr);  /* add text up to capture */
      if (addonestring(b, cs, "replacement"))
        curr = closeaddr(cs, cs->cap - 1);  /* continue after match */
      else  /* no capture value */
        curr = next;  /* keep original text in final result */
    }
    /* add last piece of text */
    luaL_addlstring(b, curr, cs->s + cs->cap->index - curr);
  }
  cs->cap++;  /* go to next capture */
}


/*
** Evaluates a capture and adds its first value to buffer 'b'; returns
** whether there was a value
*/
static int addonestring (luaL_Buffer *b, CapState *cs, const char *what) {
  switch (captype(cs->cap)) {
    case Cstring:
      stringcap(b, cs);  /* add capture directly to buffer */
      return 1;
    case Csubst:
      substcap(b, cs);  /* add capture directly to buffer */
      return 1;
    case Cacc:  /* accumulator capture? */
      return luaL_error(cs->L, "accumulator capture inside substitution capture");
    default: {
      lua_State *L = cs->L;
      int n = pushcapture(cs);
      if (n > 0) {
        if (n > 1) lua_pop(L, n - 1);  /* only one result */
        if (!lua_isstring(L, -1))
          return luaL_error(L, "invalid %s value (a %s)",
                               what, luaL_typename(L, -1));
        luaL_addvalue(b);
      }
      return n;
    }
  }
}


#if !defined(MAXRECLEVEL)
#define MAXRECLEVEL	200
#endif


/*
** Push all values of the current capture into the stack; returns
** number of values pushed
*/
static int pushcapture (CapState *cs) {
  lua_State *L = cs->L;
  int res;
  luaL_checkstack(L, 4, "too many captures");
  if (cs->reclevel++ > MAXRECLEVEL)
    return luaL_error(L, "subcapture nesting too deep");
  switch (captype(cs->cap)) {
    case Cposition: {
      lua_pushinteger(L, cs->cap->index + 1);
      cs->cap++;
      res = 1;
      break;
    }
    case Cconst: {
      pushluaval(cs);
      cs->cap++;
      res = 1;
      break;
    }
    case Carg: {
      int arg = (cs->cap++)->idx;
      if (arg + FIXEDARGS > cs->ptop)
        return luaL_error(L, "reference to absent extra argument #%d", arg);
      lua_pushvalue(L, arg + FIXEDARGS);
      res = 1;
      break;
    }
    case Csimple: {
      int k = pushnestedvalues(cs, 1);
      lua_insert(L, -k);  /* make whole match be first result */
      res = k;
      break;
    }
    case Cruntime: {
      lua_pushvalue(L, (cs->cap++)->idx);  /* value is in the stack */
      res = 1;
      break;
    }
    case Cstring: {
      luaL_Buffer b;
      luaL_buffinit(L, &b);
      stringcap(&b, cs);
      luaL_pushresult(&b);
      res = 1;
      break;
    }
    case Csubst: {
      luaL_Buffer b;
      luaL_buffinit(L, &b);
      substcap(&b, cs);
      luaL_pushresult(&b);
      res = 1;
      break;
    }
    case Cgroup: {
      if (cs->cap->idx == 0)  /* anonymous group? */
        res = pushnestedvalues(cs, 0);  /* add all nested values */
      else {  /* named group: add no values */
        nextcap(cs);  /* skip capture */
        res = 0;
      }
      break;
    }
    case Cbackref: res = backrefcap(cs); break;
    case Ctable: res = tablecap(cs); break;
    case Cfunction: res = functioncap(cs); break;
    case Cacc: res = accumulatorcap(cs); break;
    case Cnum: res = numcap(cs); break;
    case Cquery: res = querycap(cs); break;
    case Cfold: res = foldcap(cs); break;
    default: assert(0); res = 0;
  }
  cs->reclevel--;
  return res;
}


/*
** Prepare a CapState structure and traverse the entire list of
** captures in the stack pushing its results. 's' is the subject
** string, 'r' is the final position of the match, and 'ptop' 
** the index in the stack where some useful values were pushed.
** Returns the number of results pushed. (If the list produces no
** results, push the final position of the match.)
*/
int getcaptures (lua_State *L, const char *s, const char *r, int ptop) {
  Capture *capture = (Capture *)lua_touserdata(L, caplistidx(ptop));
  int n = 0;
  /* printcaplist(capture); */
  if (!isclosecap(capture)) {  /* is there any capture? */
    CapState cs;
    cs.ocap = cs.cap = capture; cs.L = L; cs.reclevel = 0;
    cs.s = s; cs.valuecached = 0; cs.ptop = ptop;
    cs.firstcap = lua_gettop(L) + 1;  /* where first value (if any) will go */
    do {  /* collect their values */
      n += pushcapture(&cs);
    } while (!isclosecap(cs.cap));
    assert(lua_gettop(L) - cs.firstcap == n - 1);
  }
  if (n == 0) {  /* no capture values? */
    lua_pushinteger(L, r - s + 1);  /* return only end position */
    n = 1;
  }
  return n;
}


