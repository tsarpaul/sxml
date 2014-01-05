#include "sxml.h"

#include <string.h>
#include <ctype.h>
#include <assert.h>

typedef unsigned UINT;
typedef int BOOL;
#define FALSE	0
#define TRUE	(!FALSE)

/*
 MARK: string
 string functions work within the memory range specified (excluding end)
 returns end if value not found
*/

static const char* str_findchr (const char* start, const char* end, int c)
{
	const char* it;

	assert (start <= end);
	assert (0 <= c && c <= 127);	/* CHAR_MAX - memchr implementation will only work when searching for ascii characters within a utf-8 string */
	
	it= (const char*) memchr (start, c, end - start);
	return (it != NULL) ? it : end;
}

static const char* str_findstr (const char* start, const char* end, const char* needle)
{
	size_t needlelen;
	int first;
	assert  (start <= end);
	
	needlelen= strlen (needle);
	assert (0 < needlelen);
	first = (unsigned char) needle[0];

	while (start + needlelen <= end)
	{
		const char* it= (const char*) memchr (start, first, (end - start) - (needlelen - 1));
		if (it == NULL)
			break;

		if (memcmp (it, needle, needlelen) == 0)
			return it;

		start= it + 1;
	}

	return end;
}

static BOOL str_startswith (const char* start, const char* end, const char* prefix)
{
	long nbytes;
	assert (start <= end);
	
	nbytes= strlen (prefix);
	if (end - start < nbytes)
		return FALSE;
	
	return memcmp (prefix, start, nbytes) == 0;
}

static BOOL str_endswith (const char* start, const char* end, const char* suffix)
{
	long nbytes;
	assert (start <= end);
	
	nbytes= strlen (suffix);
	if (end - start < nbytes)
		return FALSE;
	
	return memcmp (suffix, end - nbytes, nbytes) == 0;
}

#define ISALPHA(c)   (isalpha(((unsigned char)(c))))
#define ISSPACE(c)   (isspace(((unsigned char)(c))))

static const char* str_lspace (const char* start, const char* end)
{
	const char* it;	
	assert (start <= end);

	for (it= start; it != end && !ISSPACE (*it); it++)
		;

	return it;
}

/* left trim whitespace */
static const char* str_ltrim (const char* start, const char* end)
{
	const char* it;
	assert (start <= end);

	for (it= start; it != end && ISSPACE (*it); it++)
		;

	return it;
}

/* right trim whitespace */
static const char* str_rtrim (const char* start, const char* end)
{
	const char* it, *prev;
	assert (start <= end);

	for (it= end; start != it; it= prev)
	{
		prev= it - 1;
		if (!ISSPACE (*prev))
			return it;
	}
	
	return start;
}

/* MARK: state */

/* collect arguments in a structure for convenience */
typedef struct
{
	const char* buffer;
	UINT bufferlen;
	sxmltok_t* tokens;
	UINT num_tokens;
} sxml_args_t;

#define buffer_fromoffset(args,i)	((args)->buffer + (i))
#define buffer_tooffset(args,ptr)	(unsigned) ((ptr) - (args)->buffer)
#define buffer_getend(args) ((args)->buffer + (args)->bufferlen)

static BOOL state_pushtoken (sxml_t* state, sxml_args_t* args, sxmltype_t type, const char* start, const char* end)
{
	sxmltok_t* token;
	UINT i= state->ntokens++;
	if (args->num_tokens < state->ntokens)
		return FALSE;
	
	token= &args->tokens[i];
	token->type= type;
	token->startpos= buffer_tooffset (args, start);
	token->endpos= buffer_tooffset (args, end);
	token->size= 0;

	switch (type)
	{
		case SXML_STARTTAG:	state->taglevel++;	break;

		case SXML_ENDTAG:
			assert (0 < state->taglevel);
			state->taglevel--;
			break;

		default:
			break;
	}

	return TRUE;
}

static sxmlerr_t state_setpos (sxml_t* state, const sxml_args_t* args, const char* ptr)
{
	if (args->num_tokens < state->ntokens)
		return SXML_ERROR_TOKENSFULL;
		
	state->bufferpos= buffer_tooffset (args, ptr);
	return SXML_SUCCESS;
}

#define state_commit(dest,src) memcpy ((dest), (src), sizeof (sxml_t))

/*
 MARK: parse
 
 SXML does minimal validation of the input data
 SXML_ERROR_XMLSTRICT is returned if some simple XML validation tests fail
 SXML_ERROR_XMLINVALID is instead returned if the invalid XML data is serious enough to prevent the parser from continuing
 we currently make no difference between these two - but they are marked diffently in case we wish to do so in the future
*/

#define SXML_ERROR_XMLSTRICT	SXML_ERROR_XMLINVALID

#define TAG_LEN(str)	(sizeof (str) - 1)
#define TAG_MINSIZE	3

static sxmlerr_t parse_attributes (sxml_t* state, sxml_args_t* args, const char* start, const char* end)
{
	sxmltok_t* token;
	const char* name;

	assert (0 < state->ntokens);
	token= args->tokens + (state->ntokens - 1);


	name= str_ltrim (start, end);
	while (name != end)
	{
		/* attribute name */
		const char* eq, *space, *quot, *value;
		if (!ISALPHA(*name))
			return SXML_ERROR_XMLSTRICT;

		eq= str_findchr (name, end, '=');
		if (eq == end)
			return SXML_ERROR_XMLINVALID;

		space= str_rtrim (name, eq);
		if (!state_pushtoken (state, args, SXML_CDATA, name, space))
			return SXML_ERROR_TOKENSFULL;

		/* attribute value */
		quot= str_ltrim (eq + 1, end);
		if (quot == end || !(*quot == '\'' || *quot == '"'))
			return SXML_ERROR_XMLINVALID;

		value= quot + 1;
		quot= str_findchr (value, end, *quot);
		if (quot == end)
			return SXML_ERROR_XMLINVALID;

		if (!state_pushtoken (state, args, SXML_CHARACTER, value, quot))
			return SXML_ERROR_TOKENSFULL;

		/* --- */
		token->size+= 2;
		name= str_ltrim (quot + 1, end);
	}

	return SXML_SUCCESS;
}

static sxmlerr_t parse_comment (sxml_t* state, sxml_args_t* args)
{
	static const char STARTTAG[]= "<!--";
	static const char ENDTAG[]= "-->";

	const char* dash;
	const char* start= buffer_fromoffset (args, state->bufferpos);
	const char* end= buffer_getend (args);
	if (end - start < TAG_LEN (STARTTAG))
		return SXML_ERROR_BUFFERDRY;

	if (!str_startswith (start, end, STARTTAG))
		return SXML_ERROR_XMLINVALID;

	start+= TAG_LEN (STARTTAG);
	dash= str_findstr (start, end, ENDTAG);
	if (dash == end)
		return SXML_ERROR_BUFFERDRY;

	state_pushtoken (state, args, SXML_COMMENT, start, dash);
	return state_setpos (state, args, dash + TAG_LEN (ENDTAG));
}

static sxmlerr_t parse_instruction (sxml_t* state, sxml_args_t* args)
{
	static const char STARTTAG[]= "<?";
	static const char ENDTAG[]= "?>";

	const char* quest, *space;
	const char* start= buffer_fromoffset (args, state->bufferpos);
	const char* end= buffer_getend (args);
	assert (TAG_MINSIZE <= end - start);

	if (!str_startswith (start, end, STARTTAG))
		return SXML_ERROR_XMLINVALID;

	start+= TAG_LEN (STARTTAG);
	quest= str_findstr (start, end, ENDTAG);
	if (quest == end)
		return SXML_ERROR_BUFFERDRY;

	space= str_lspace (start, quest);
	if (!state_pushtoken (state, args, SXML_INSTRUCTION, start, space))
		return SXML_ERROR_TOKENSFULL;

	parse_attributes (state, args, space, quest);
	return state_setpos (state, args, quest + TAG_LEN (ENDTAG));
}

static sxmlerr_t parse_doctype (sxml_t* state, sxml_args_t* args)
{
	static const char STARTTAG[]= "<!DOCTYPE";
	static const char ENDTAG[]= "]>";

	const char* bracket;
	const char* start= buffer_fromoffset (args, state->bufferpos);
	const char* end= buffer_getend (args);
	if (end - start < TAG_LEN (STARTTAG))
		return SXML_ERROR_BUFFERDRY;

	if (!str_startswith (start, end, STARTTAG))
		return SXML_ERROR_BUFFERDRY;

	start+= TAG_LEN (STARTTAG);
	bracket= str_findstr (start, end, ENDTAG);
	if (bracket == end)
		return SXML_ERROR_BUFFERDRY;

	state_pushtoken (state, args, SXML_DOCTYPE, start, bracket);
	return state_setpos (state, args, bracket + TAG_LEN (ENDTAG));
}

static sxmlerr_t parse_start (sxml_t* state, sxml_args_t* args)
{	
	BOOL empty;
	sxmlerr_t err;
	const char* gt, *name, *space;
	const char* start= buffer_fromoffset (args, state->bufferpos);
	const char* end= buffer_getend (args);
	assert (TAG_MINSIZE <= end - start);

	if (!(start[0] == '<' && ISALPHA (start[1])))
		return SXML_ERROR_XMLINVALID;

	start++;		
	gt= str_findchr (start, end, '>');
	if (gt == end)
		return SXML_ERROR_BUFFERDRY;

	empty= str_endswith (start, gt + 1, "/>");
	end= (empty) ? gt - 1 : gt;

	/* --- */

	name= start;
	space= str_lspace (name, end);
	if (!state_pushtoken (state, args, SXML_STARTTAG, name, space))
		return SXML_ERROR_TOKENSFULL;

	err= parse_attributes (state, args, space, end);
	if (err != SXML_SUCCESS)
		return err;

	/* --- */

	if (empty)
		state_pushtoken (state, args, SXML_ENDTAG, name, space);

	return state_setpos (state, args, gt + 1);
}

static sxmlerr_t parse_end (sxml_t* state, sxml_args_t* args)
{
	const char* gt, *space;
	const char* start= buffer_fromoffset (args, state->bufferpos);
	const char* end= buffer_getend (args);
	assert (TAG_MINSIZE <= end - start);

	if (!(str_startswith (start, end, "</") && ISALPHA (start[2])))
		return SXML_ERROR_XMLINVALID;

	start+= 2;	
	gt= str_findchr (start, end, '>');
	if (gt == end)
		return SXML_ERROR_BUFFERDRY;

	/* test for no characters beyond elem name */
	space= str_lspace (start, gt);
	if (str_ltrim (space, gt) != gt)
		return SXML_ERROR_XMLSTRICT;

	state_pushtoken (state, args, SXML_ENDTAG, start, space);
	return state_setpos (state, args, gt + 1);
}

static sxmlerr_t parse_cdata (sxml_t* state, sxml_args_t* args)
{
	static const char STARTTAG[]= "<![CDATA[";
	static const char ENDTAG[]= "]]>";

	const char* bracket;
	const char* start= buffer_fromoffset (args, state->bufferpos);
	const char* end= buffer_getend (args);
	if (end - start < TAG_LEN (STARTTAG))
		return SXML_ERROR_BUFFERDRY;

	if (!str_startswith (start, end, STARTTAG))
		return SXML_ERROR_XMLINVALID;

	start+= TAG_LEN (STARTTAG);
	bracket= str_findstr (start, end, ENDTAG);
	if (bracket == end)
		return SXML_ERROR_BUFFERDRY;

	state_pushtoken (state, args, SXML_CDATA, start, bracket);
	return state_setpos (state, args, bracket + TAG_LEN (ENDTAG));
}

static sxmlerr_t parse_characters (sxml_t* state, sxml_args_t* args)
{
	const char* start= buffer_fromoffset (args, state->bufferpos);
	const char* end= buffer_getend (args);

	const char* lt= str_findchr (start, end, '<');
	if (lt == end)
		return SXML_ERROR_BUFFERDRY;

	if (lt != start)
		state_pushtoken (state, args, SXML_CHARACTER, start, lt);

	return state_setpos (state, args, lt);
}

/*
 MARK: sxml
 Public API inspired by the JSON parser jsmn ( http://zserge.com/jsmn.html )
*/

void sxml_init (sxml_t *state)
{
    state->bufferpos= 0;
    state->ntokens= 0;
	state->taglevel= 0;
}

#define ROOT_FOUND(state)	(0 < (state)->taglevel)
#define ROOT_PARSED(state)	((state)->taglevel == 0)

sxmlerr_t sxml_parse(sxml_t *state, const char *buffer, UINT bufferlen, sxmltok_t tokens[], UINT num_tokens)
{
	sxml_t temp= *state;
	const char* end= buffer + bufferlen;
	
	sxml_args_t args;
	args.buffer= buffer;
	args.bufferlen= bufferlen;
	args.tokens= tokens;
	args.num_tokens= num_tokens;

	/* --- */

	while (!ROOT_FOUND (&temp))
	{
		sxmlerr_t err;
		const char* start= buffer_fromoffset (&args, temp.bufferpos);
		const char* lt= str_ltrim (start, end);
		if (end - lt < TAG_MINSIZE)
			return SXML_ERROR_BUFFERDRY;

		if (*lt != '<')
			return SXML_ERROR_XMLINVALID;

		state_setpos (&temp, &args, lt);
		state_commit (state, &temp);

		/* --- */

		switch (lt[1])
		{
		case '?':	err= parse_instruction (&temp, &args);	break;
		case '!':	err= parse_doctype (&temp, &args);	break;
		default:	err= parse_start (&temp, &args);	break;
		}

		if (err != SXML_SUCCESS)
			return err;

		state_commit (state, &temp);
	}

	/* --- */

	while (!ROOT_PARSED (&temp))
	{
		const char* lt;
		sxmlerr_t err= parse_characters (&temp, &args);
		if (err != SXML_SUCCESS)
			return err;

		state_commit (state, &temp);

		/* --- */

		lt= buffer_fromoffset (&args, temp.bufferpos);
		assert (*lt == '<');		
		if (end - lt < TAG_MINSIZE)
			return SXML_ERROR_BUFFERDRY;

		switch (lt[1])
		{
		case '?':	err= parse_instruction (&temp, &args);		break;
		case '/':	err= parse_end (&temp, &args);	break;
		case '!':	err= (lt[2] == '-') ? parse_comment (&temp, &args) : parse_cdata (&temp, &args);	break;
		default:	err= parse_start (&temp, &args);	break;
		}

		if (err != SXML_SUCCESS)
			return err;

		state_commit (state, &temp);
	}

	return SXML_SUCCESS;
}
