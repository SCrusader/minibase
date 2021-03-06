#include <string.h>
#include <format.h>

#include "msh.h"

/* The parser is kinda scary and I should really feel bad about writing
   it this way. Maybe at some point it should be rewritten. */

#define SEP   0x0	/* .             any inter-token space            */
#define LEAD  0x1       /* ech.          leading arg *or* variable name   */
#define ARG   0x2	/* echo Hel.     any arg that's not a variable    */
#define DQUOT 0x3       /* echo "He.     double-quoted context            */
#define SQUOT 0x4       /* echo 'He.     single-quoted context            */
#define SLASH 0x5       /* echo "\.      character following backslash    */
#define VSIGN 0x6       /* echo $.       first char of var reference      */
#define VCONT 0x7       /* echo $fo.     non-first char of var reference  */
#define COMM  0x8       /* # some co.    commented-out context            */
#define VALUE 0x9       /* var=va.       rhs in variable definition       */
#define TRAIL 0xa       /* var=val .     whatever follows variable def    */

static void dispatch(CTX, char c);

static void set_state(CTX, int st)
{
	ctx->state = (ctx->state & ~0xFF) | (st & 0xFF);
}

static void push_state(CTX, int st)
{
	ctx->state = (ctx->state << 8) | (st & 0xFF);
}

static void pop_state(CTX)
{
	ctx->state = (ctx->state >> 8);
}

static void start_var(CTX)
{
	ctx->var = ctx->hptr;
}

static void end_var(CTX)
{
	/* terminate and substitute $VAR */
	*(ctx->hptr) = '\0';	
	ctx->hptr = ctx->var;
	ctx->var = NULL;

	char *var = ctx->hptr, *val;

	if(!(val = valueof(ctx, var)))
		fatal(ctx, "undefined variable", var);

	long vlen = strlen(val);
	char* spc = halloc(ctx, vlen);
	memcpy(spc, val, vlen);
}

static void add_char(CTX, char c)
{
	char* spc = halloc(ctx, 1);
	*spc = c;
}

static void end_arg(CTX)
{
	add_char(ctx, 0);

	ctx->argc++;
}

static char** put_argv(CTX)
{
	int argn = ctx->argc;
	char* base = ctx->csep;
	char* bend = ctx->hptr;

	char** argv = halloc(ctx, (argn+1)*sizeof(char*));
	int argc = 0;

	argv[0] = ctx->csep;
	char* p;

	int sep = 1;
	for(p = base; p < bend; p++) {
		if(sep && argc < argn)
			argv[argc++] = p;
		sep = !*p;
	}

	if(argc != argn)
		fatal(ctx, "miscounted args", NULL);

	argv[argc] = NULL;

	return argv;
}

static void end_val(CTX)
{
	ctx->argc++;

	if(ctx->argc != 2)
		goto out;
	
	add_char(ctx, 0);
	char** argv = put_argv(ctx);

	define(ctx, argv[0], argv[1]); /* may damage heap, argv, csep! */
out:
	hrev(ctx, CSEP);
	ctx->argc = 0;
}

static void end_cmd(CTX)
{
	if(!ctx->argc) return;

	ctx->argv = put_argv(ctx);
	ctx->argp = 1;

	command(ctx); /* may damage heap, argv, csep! */

	hrev(ctx, CSEP);

	ctx->argc = 0;
	ctx->argv = NULL;
	ctx->argp = 0;
}

static void parse_sep(CTX, char c)
{
	switch(c) {
		case '\0':
		case ' ':
		case '\t':
			break;
		case '#':
			end_cmd(ctx);
			set_state(ctx, COMM);
			break;
		case '\n':
			end_cmd(ctx);
			set_state(ctx, SEP);
			break;
		default:
			set_state(ctx, ctx->argc ? ARG : LEAD);
			dispatch(ctx, c);
	};
}

static void parse_lead(CTX, char c)
{
	switch(c) {
		case 'a'...'z':
		case 'A'...'Z':
		case '_':
			add_char(ctx, c);
			break;
		case '=':
			end_arg(ctx);
			set_state(ctx, SEP);
			set_state(ctx, VALUE);
			break;
		default:
			set_state(ctx, ARG);
			dispatch(ctx, c);
	}
}

static void parse_arg(CTX, char c)
{
	switch(c) {
		case '\0':
		case ' ':
		case '\t':
			end_arg(ctx);
			set_state(ctx, SEP);
			break;
		case '\n':
			end_arg(ctx);
			end_cmd(ctx);
			set_state(ctx, SEP);
			break;
		case '$':
			start_var(ctx);
			push_state(ctx, VSIGN);
			break;
		case '"':
			push_state(ctx, DQUOT);
			break;
		case '\'':
			push_state(ctx, SQUOT);
			break;
		case '\\':
			push_state(ctx, SLASH);
			break;
		default:
			add_char(ctx, c);
	}
}

static void parse_dquote(CTX, char c)
{
	switch(c) {
		case '"':
			pop_state(ctx);
			break;
		case '$':
			start_var(ctx);
			push_state(ctx, VSIGN);
			break;
		case '\\':
			push_state(ctx, SLASH);
			break;
		case '\0':
			fatal(ctx, "unbalanced quote", NULL);
		default:
			add_char(ctx, c);
	}
}

static void parse_squote(CTX, char c)
{
	switch(c) {
		case '\'':
			pop_state(ctx);
			break;
		case '\0':
			fatal(ctx, "unbalanced quote", NULL);
		default:
			add_char(ctx, c);
	}
}

static void parse_slash(CTX, char c)
{
	switch(c) {
		case '\0': fatal(ctx, "trailing backslash", NULL);
		case '\n': c = ' '; break;
		case 't': c = '\t'; break;
		case 'n': c = '\n'; break;
	};
	
	add_char(ctx, c);
	pop_state(ctx);
}

static void parse_vsign(CTX, char c)
{
	switch(c) {
		case 'a'...'z':
		case 'A'...'Z':
		case '_':
			add_char(ctx, c);
			set_state(ctx, VCONT);
			break;
		case '"':
		case '\'':
		case '\\':
		case '\0':
		case '\n':
			fatal(ctx, "invalid syntax", NULL);
		default:
			add_char(ctx, c);
			end_var(ctx);
			pop_state(ctx);
	}
}

static void parse_comm(CTX, char c)
{
	if(c == '\n')
		set_state(ctx, SEP);
}

static void parse_vcont(CTX, char c)
{
	switch(c) {
		case 'a'...'z':
		case 'A'...'Z':
		case '0'...'9':
		case '_':
			add_char(ctx, c);
			break;
		default:
			end_var(ctx);
			pop_state(ctx);
			dispatch(ctx, c);
	}
}

static void parse_value(CTX, char c)
{
	switch(c) {
		case '\0': /* foo=|EOF| */
		case ' ':
		case '\t':
			end_val(ctx);
			set_state(ctx, TRAIL);
			break;
		case '\n':
			end_val(ctx);
			set_state(ctx, SEP);
			break;
		case '$':
			start_var(ctx);
			push_state(ctx, VSIGN);
			break;
		case '"':
			push_state(ctx, DQUOT);
			break;
		case '\'':
			push_state(ctx, SQUOT);
			break;
		case '\\':
			push_state(ctx, SLASH);
			break;
		default:
			add_char(ctx, c);
	}
}

static void parse_trail(CTX, char c)
{
	switch(c) {
		case ' ':
		case '\t':
			break;
		case '\n':
			set_state(ctx, SEP);
			break;
		default:
			fatal(ctx, "trailing characters", NULL);
	}
}

static void dispatch(CTX, char c)
{
	switch(ctx->state & 0xFF) {
		case SEP: parse_sep(ctx, c); break;
		case ARG: parse_arg(ctx, c); break;
		case DQUOT: parse_dquote(ctx, c); break;
		case SQUOT: parse_squote(ctx, c); break;
		case SLASH: parse_slash(ctx, c); break;
		case VSIGN: parse_vsign(ctx, c); break;
		case VCONT: parse_vcont(ctx, c); break;
		case COMM: parse_comm(ctx, c); break;

		case LEAD: parse_lead(ctx, c); break;
		case VALUE: parse_value(ctx, c); break;
		case TRAIL: parse_trail(ctx, c); break;

		default: fatal(ctx, "bad internal state", NULL);
	}
}

void parse(CTX, char* buf, int len)
{
	char* end = buf + len;
	char* p;

	for(p = buf; p < end; p++) {
		if(!*p) fatal(ctx, "stray \\0 in script", NULL);
		dispatch(ctx, *p);
		if(*p == '\n' && ctx->line)
			ctx->line++;
	}
}

void pfini(CTX)
{
	dispatch(ctx, '\n');

	if(ctx->state)
		fatal(ctx, "unexpected EOF", NULL);
}
