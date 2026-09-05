/*
 * MIT License
 * Copyright (c) 2010 Serge Zaitsev
 */
#ifndef JSMN_H
#define JSMN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef JSMN_STATIC
#define JSMN_API static
#else
#define JSMN_API extern
#endif

typedef enum {
    JSMN_UNDEFINED = 0,
    JSMN_OBJECT = 1 << 0,
    JSMN_ARRAY = 1 << 1,
    JSMN_STRING = 1 << 2,
    JSMN_PRIMITIVE = 1 << 3
} jsmntype_t;

enum jsmnerr {
    JSMN_ERROR_NOMEM = -1,
    JSMN_ERROR_INVAL = -2,
    JSMN_ERROR_PART = -3
};

typedef struct jsmntok {
    jsmntype_t type;
    int start;
    int end;
    int size;
#ifdef JSMN_PARENT_LINKS
    int parent;
#endif
} jsmntok_t;

typedef struct jsmn_parser {
    unsigned int pos;
    unsigned int toknext;
    int toksuper;
} jsmn_parser;

JSMN_API void jsmn_init(jsmn_parser *parser);
JSMN_API int jsmn_parse(jsmn_parser *parser, const char *js, size_t len,
                        jsmntok_t *tokens, unsigned int num_tokens);

#ifndef JSMN_HEADER
static jsmntok_t *jsmn_alloc_token(jsmn_parser *parser, jsmntok_t *tokens,
                                   size_t num_tokens)
{
    jsmntok_t *token;

    if (parser->toknext >= num_tokens)
        return NULL;
    token = &tokens[parser->toknext++];
    token->start = token->end = -1;
    token->size = 0;
#ifdef JSMN_PARENT_LINKS
    token->parent = -1;
#endif
    return token;
}

static void jsmn_fill_token(jsmntok_t *token, jsmntype_t type,
                            int start, int end)
{
    token->type = type;
    token->start = start;
    token->end = end;
    token->size = 0;
}

static int jsmn_parse_primitive(jsmn_parser *parser, const char *js,
                                size_t len, jsmntok_t *tokens,
                                size_t num_tokens)
{
    jsmntok_t *token;
    int start = (int)parser->pos;

    for (; parser->pos < len && js[parser->pos] != '\0'; parser->pos++) {
        switch (js[parser->pos]) {
        case '\t': case '\r': case '\n': case ' ': case ',': case ']': case '}':
            goto found;
        default:
            break;
        }
        if ((unsigned char)js[parser->pos] < 32U ||
            (unsigned char)js[parser->pos] >= 127U) {
            parser->pos = (unsigned int)start;
            return JSMN_ERROR_INVAL;
        }
    }
found:
    if (tokens == NULL) {
        parser->pos--;
        return 0;
    }
    token = jsmn_alloc_token(parser, tokens, num_tokens);
    if (token == NULL) {
        parser->pos = (unsigned int)start;
        return JSMN_ERROR_NOMEM;
    }
    jsmn_fill_token(token, JSMN_PRIMITIVE, start, (int)parser->pos);
#ifdef JSMN_PARENT_LINKS
    token->parent = parser->toksuper;
#endif
    parser->pos--;
    return 0;
}

static int jsmn_parse_string(jsmn_parser *parser, const char *js,
                             size_t len, jsmntok_t *tokens,
                             size_t num_tokens)
{
    jsmntok_t *token;
    int start = (int)parser->pos;

    parser->pos++;
    for (; parser->pos < len && js[parser->pos] != '\0'; parser->pos++) {
        char character = js[parser->pos];

        if (character == '"') {
            if (tokens == NULL)
                return 0;
            token = jsmn_alloc_token(parser, tokens, num_tokens);
            if (token == NULL) {
                parser->pos = (unsigned int)start;
                return JSMN_ERROR_NOMEM;
            }
            jsmn_fill_token(token, JSMN_STRING, start + 1, (int)parser->pos);
#ifdef JSMN_PARENT_LINKS
            token->parent = parser->toksuper;
#endif
            return 0;
        }
        if (character == '\\' && parser->pos + 1U < len) {
            int index;

            parser->pos++;
            switch (js[parser->pos]) {
            case '"': case '/': case '\\': case 'b': case 'f':
            case 'r': case 'n': case 't':
                break;
            case 'u':
                parser->pos++;
                for (index = 0; index < 4 && parser->pos < len; index++) {
                    char hex = js[parser->pos];
                    if (!((hex >= '0' && hex <= '9') ||
                          (hex >= 'A' && hex <= 'F') ||
                          (hex >= 'a' && hex <= 'f'))) {
                        parser->pos = (unsigned int)start;
                        return JSMN_ERROR_INVAL;
                    }
                    parser->pos++;
                }
                parser->pos--;
                break;
            default:
                parser->pos = (unsigned int)start;
                return JSMN_ERROR_INVAL;
            }
        }
    }
    parser->pos = (unsigned int)start;
    return JSMN_ERROR_PART;
}

JSMN_API int jsmn_parse(jsmn_parser *parser, const char *js, size_t len,
                        jsmntok_t *tokens, unsigned int num_tokens)
{
    int result;
    int index;
    int count = (int)parser->toknext;
    jsmntok_t *token;

    for (; parser->pos < len && js[parser->pos] != '\0'; parser->pos++) {
        char character = js[parser->pos];

        switch (character) {
        case '{': case '[':
            count++;
            if (tokens == NULL)
                break;
            token = jsmn_alloc_token(parser, tokens, num_tokens);
            if (token == NULL)
                return JSMN_ERROR_NOMEM;
            if (parser->toksuper != -1)
                tokens[parser->toksuper].size++;
            token->type = character == '{' ? JSMN_OBJECT : JSMN_ARRAY;
            token->start = (int)parser->pos;
            parser->toksuper = (int)parser->toknext - 1;
            break;
        case '}': case ']':
            if (tokens == NULL)
                break;
            for (index = (int)parser->toknext - 1; index >= 0; index--) {
                token = &tokens[index];
                if (token->start != -1 && token->end == -1) {
                    jsmntype_t expected = character == '}' ? JSMN_OBJECT : JSMN_ARRAY;
                    if (token->type != expected)
                        return JSMN_ERROR_INVAL;
                    token->end = (int)parser->pos + 1;
                    parser->toksuper = -1;
                    break;
                }
            }
            if (index == -1)
                return JSMN_ERROR_INVAL;
            for (; index >= 0; index--) {
                token = &tokens[index];
                if (token->start != -1 && token->end == -1) {
                    parser->toksuper = index;
                    break;
                }
            }
            break;
        case '"':
            result = jsmn_parse_string(parser, js, len, tokens, num_tokens);
            if (result < 0)
                return result;
            count++;
            if (parser->toksuper != -1 && tokens != NULL)
                tokens[parser->toksuper].size++;
            break;
        case '\t': case '\r': case '\n': case ' ':
            break;
        case ':':
            parser->toksuper = (int)parser->toknext - 1;
            break;
        case ',':
            if (tokens != NULL && parser->toksuper != -1 &&
                tokens[parser->toksuper].type != JSMN_ARRAY &&
                tokens[parser->toksuper].type != JSMN_OBJECT) {
                for (index = (int)parser->toknext - 1; index >= 0; index--) {
                    if ((tokens[index].type == JSMN_ARRAY ||
                         tokens[index].type == JSMN_OBJECT) &&
                        tokens[index].start != -1 && tokens[index].end == -1) {
                        parser->toksuper = index;
                        break;
                    }
                }
            }
            break;
        default:
            result = jsmn_parse_primitive(parser, js, len, tokens, num_tokens);
            if (result < 0)
                return result;
            count++;
            if (parser->toksuper != -1 && tokens != NULL)
                tokens[parser->toksuper].size++;
            break;
        }
    }
    if (tokens != NULL) {
        for (index = (int)parser->toknext - 1; index >= 0; index--) {
            if (tokens[index].start != -1 && tokens[index].end == -1)
                return JSMN_ERROR_PART;
        }
    }
    return count;
}

JSMN_API void jsmn_init(jsmn_parser *parser)
{
    parser->pos = 0;
    parser->toknext = 0;
    parser->toksuper = -1;
}
#endif

#ifdef __cplusplus
}
#endif
#endif
