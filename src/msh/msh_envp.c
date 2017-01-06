#include <format.h>
#include <string.h>
#include <util.h>

#include "msh.h"

static char* match(char* envline, char* var)
{
	char* a = envline;
	char* b = var;

	while(*a == *b) {
		a++;
		b++;
	} if(*b || *a != '=')
		return NULL;

	return a + 1;
}

char* valueof(struct sh* ctx, char* var)
{
	char** e;
	char* v;

	for(e = ctx->envp; *e; e++)
		if((v = match(*e, var)))
			return v;

	return "";
}

static void maybe_init_env(struct sh* ctx)
{
	char** e;
	struct envptr* p;

	if(ctx->esep)
		return;

	hrev(ctx, HEAP);

	for(e = ctx->envp; *e; e++) {
		p = halloc(ctx, sizeof(*p));
		p->len = sizeof(*p);
		p->type = ENVPTR;
		p->ref = *e;
	}

	hset(ctx, ESEP);
}

/* This function makes little sense at the first glance because
   it's just happens the common part of otherwise unrelated rebuild_envp()
   and del_env_entry(). Basically just a loop over env entries. */

typedef int (*envf)(struct sh* ctx, struct env* es, char* str, char* var);

static int foreach_env(struct sh* ctx, envf func, char* var)
{
	char* ptr = ctx->heap;
	char* end = ctx->esep;
	char* p = ptr;

	ctx->envp = (char**)ctx->hptr;

	while(p < end) {
		struct env* es = (struct env*) p;
		char* str;

		p += es->len;

		if(!es->len)
			break;
		if(es->type == ENVSTR)
			str = es->payload;
		else if(es->type == ENVPTR)
			str = ((struct envptr*)es)->ref;
		else
			continue;

		if(func(ctx, es, str, var))
			return 1;
	}

	return 0;
}

static int add_env_ptr(struct sh* ctx, struct env* _, char* str, char* __)
{
	char** ptr = halloc(ctx, sizeof(char*));
	*ptr = str;
	return 0;
}

static void rebuild_envp(struct sh* ctx)
{
	foreach_env(ctx, add_env_ptr, NULL);
	add_env_ptr(ctx, NULL, NULL, NULL);
}

static int match_del_entry(struct sh* ctx, struct env* es, char* str, char* var)
{
	if(match(str, var)) {
		es->type = ENVDEL;
		return 1;
	} else {
		return 0;
	}
}

static int del_env_entry(struct sh* ctx, char* var)
{
	return foreach_env(ctx, match_del_entry, var);
}

void define(struct sh* ctx, char* var, char* val)
{
	maybe_init_env(ctx);

	hrev(ctx, ESEP);

	del_env_entry(ctx, var);

	int len = strlen(var) + strlen(val) + 1;
	int total = sizeof(struct env) + len + 1;
	struct env* es = halloc(ctx, total);

	es->len = total;
	es->type = ENVSTR;

	char* p = es->payload;
	char* e = p + len;

	p = fmtstr(p, e, var);
	p = fmtstr(p, e, "=");
	p = fmtstr(p, e, val);
	*p++ = '\0';

	hset(ctx, ESEP);

	rebuild_envp(ctx);

	hset(ctx, CSEP);
}

void undef(struct sh* ctx, char* var)
{
	maybe_init_env(ctx);

	hrev(ctx, ESEP);

	if(del_env_entry(ctx, var))
		rebuild_envp(ctx);

	hset(ctx, CSEP);
}