/*
 * Byte-by-byte parser using a finite state machine
 * Copyright (c) 2024 Marc Delling
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

enum
{
    STATE_ITEM,
    STATE_OBJECT_KEY,
    STATE_OBJECT_KEY_PARSED,
    STATE_OBJECT_VALUE,
    STATE_OBJECT_VALUE_PARSED,
    STATE_ARRAY_VALUE,
    STATE_ARRAY_VALUE_PARSED,
    STATE_STRING,
    STATE_SPECIAL_CHAR,
    STATE_NUMBER,
    STATE_TRUE,
    STATE_FALSE,
    STATE_NULL
};

enum
{
    STATE_RETURN_FAIL = -1,
    STATE_RETURN_CONT,
    STATE_RETURN_DONE
};

static int putbyte(cJSON *const item, char byte);

// This is thread-unsafe but massively reduces (re-)allocations
// FIXME: create a parsing context and add a dynamically allocated scratch buffer there
static size_t scratch_it = 0;
static char scratchbuf[128]; // sizeof(scratchbuf)-1 is also the maximum key or string length, may not be less

// FEATURE: deliberately misusing cJSON attribute valueint to keep the parser state, as it is never written when parsing
#define STATE item->valueint
#define CHILDSTATE item->child->valueint
#define MALLOC malloc

// FIXME: duplicated cJSON_New_Item because of visibility
static cJSON *cJSON_New_Item_2()
{
    cJSON *node = (cJSON *)MALLOC(sizeof(cJSON));
    if (node)
    {
        memset(node, '\0', sizeof(cJSON));
    }
    return node;
}

CJSON_PUBLIC(cJSON *) cJSON_Put(cJSON *item, const char byte, bool *complete)
{
    int retval;

    if (item == NULL)
    {
        item = cJSON_New_Item_2();
        if (item == NULL)
        {
            goto fail;
        }
    }

    retval = putbyte(item, byte);

    if (retval == STATE_RETURN_FAIL)
    {
        scratch_it = 0;
        // if (byte != '\n')
        //     printf("parse fail at '%c'\n", byte);
        goto fail;
    }
    else if (retval == STATE_RETURN_DONE)
    {
        scratch_it = 0;
        if (complete != NULL)
        {
            *complete = true;
        }
    }

    return item;

fail:
    if (item != NULL)
    {
        cJSON_Delete(item);
        item = NULL;
    }

    return item;
}

static int string_append(cJSON *item, char byte)
{
    if (scratch_it >= sizeof(scratchbuf))
    {
        return STATE_RETURN_FAIL;
    }

    scratchbuf[scratch_it] = byte;

    if (byte == '\0')
    {
        item->valuestring = MALLOC(scratch_it + sizeof(""));
        if (item->valuestring == NULL)
        {
            return STATE_RETURN_FAIL;
        }
        memcpy(item->valuestring, scratchbuf, scratch_it + sizeof(""));
        scratch_it = 0;

        return STATE_RETURN_DONE;
    }

    scratch_it++;

    return STATE_RETURN_CONT;
}

static inline int is_whitespace(char byte)
{
    return (byte <= 0x20);
}

static int state_object_key(cJSON *item, char byte)
{
    int retval;

    if (item->child == NULL)
    {
        if (is_whitespace(byte))
        {
            return STATE_RETURN_CONT;
        }
        else if (byte == '}')
        {
            return STATE_RETURN_DONE;
        }
        else if (byte != '"')
        {
            return STATE_RETURN_FAIL;
        }

        item->child = cJSON_New_Item_2();

        if (item->child == NULL)
        {
            return STATE_RETURN_FAIL;
        }

        item->child->type = cJSON_String;

        CHILDSTATE = STATE_STRING;

        return STATE_RETURN_CONT;
    }

    retval = putbyte(item->child, byte);

    if (retval == STATE_RETURN_DONE)
    {
        item->child->string = item->child->valuestring;
        item->child->valuestring = NULL;

        STATE = STATE_OBJECT_KEY_PARSED;
        CHILDSTATE = STATE_ITEM;

        return STATE_RETURN_CONT;
    }

    return retval;
}

static int state_object_key_parsed(cJSON *item, char byte)
{
    if (is_whitespace(byte))
    {
        return STATE_RETURN_CONT;
    }
    else if (byte == ':')
    {
        STATE = STATE_OBJECT_VALUE;
        return STATE_RETURN_CONT;
    }
    else
    {
        return STATE_RETURN_FAIL;
    }
}

static int state_object_array_value(cJSON *item, char byte)
{
    int retval;

    if (item->child == NULL)
    {
        if (is_whitespace(byte))
        {
            return STATE_RETURN_CONT;
        }
        else if (byte == ']')
        {
            return STATE_RETURN_DONE;
        }

        item->child = cJSON_New_Item_2();

        if (item->child == NULL)
        {
            return STATE_RETURN_FAIL;
        }
    }

    retval = putbyte(item->child, byte);

    if (retval == STATE_RETURN_DONE)
    {
        STATE = (STATE == STATE_ARRAY_VALUE) ? STATE_ARRAY_VALUE_PARSED : STATE_OBJECT_VALUE_PARSED;
        // state_number already consumed the terminating char, so put it back again
        if (item->child->type == cJSON_Number)
        {
            return putbyte(item, byte);
        }
        else
        {
            return STATE_RETURN_CONT;
        }
    }

    return retval;
}

static int state_object_array_value_parsed(cJSON *item, char byte)
{
    if (is_whitespace(byte))
    {
        return STATE_RETURN_CONT;
    }
    else if (byte == ',')
    {
        item->child->next = cJSON_New_Item_2();
        if (item->child->next == NULL)
        {
            return STATE_RETURN_FAIL;
        }
        item->child->next->prev = item->child;
        item->child = item->child->next;

        STATE = (STATE == STATE_ARRAY_VALUE_PARSED) ? STATE_ARRAY_VALUE : STATE_OBJECT_KEY;

        return STATE_RETURN_CONT;
    }
    else if ((byte == ']' && STATE == STATE_ARRAY_VALUE_PARSED) || (byte == '}' && STATE == STATE_OBJECT_VALUE_PARSED))
    {
        while (item->child->prev)
        {
            item->child = item->child->prev;
        }
        return STATE_RETURN_DONE;
    }
    else
    {
        return STATE_RETURN_FAIL;
    }
}

static int state_item(cJSON *item, char byte)
{
    switch (byte)
    {
    case '{':
        STATE = STATE_OBJECT_KEY;
        item->type = cJSON_Object;
        return STATE_RETURN_CONT;
    case '[':
        STATE = STATE_ARRAY_VALUE;
        item->type = cJSON_Array;
        return STATE_RETURN_CONT;
    case '"':
        STATE = STATE_STRING;
        item->type = cJSON_String;
        return STATE_RETURN_CONT;
    case 't':
        STATE = STATE_TRUE;
        item->type = cJSON_True;
        scratchbuf[scratch_it++] = byte;
        return STATE_RETURN_CONT;
    case 'f':
        STATE = STATE_FALSE;
        item->type = cJSON_False;
        scratchbuf[scratch_it++] = byte;
        return STATE_RETURN_CONT;
    case 'n':
        STATE = STATE_NULL;
        item->type = cJSON_NULL;
        scratchbuf[scratch_it++] = byte;
        return STATE_RETURN_CONT;
    case '-':
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
        STATE = STATE_NUMBER;
        item->type = cJSON_Number;
        scratchbuf[scratch_it++] = byte;
        return STATE_RETURN_CONT;
    default:
        return STATE_RETURN_FAIL;
    }
}

static int state_string(cJSON *item, char byte)
{
    if (byte == '"')
    {
        string_append(item, '\0');
        return STATE_RETURN_DONE;
    }
    else if (byte == '\\')
    {
        STATE = STATE_SPECIAL_CHAR;
    }
    else
    {
        string_append(item, byte);
    }

    return STATE_RETURN_CONT;
}

static int state_special_char(cJSON *item, char byte)
{
    switch (byte)
    {
    case 'b':
        string_append(item, '\b');
        break;

    case 'f':
        string_append(item, '\f');
        break;

    case 'n':
        string_append(item, '\n');
        break;

    case 'r':
        string_append(item, '\r');
        break;

    case 't':
        string_append(item, '\t');
        break;

    case '"':
    case '\\':
    case '/':
        string_append(item, byte);
        break;

        // FIXME: case 'u' not handled: followed by four hex digits

    default:
        return STATE_RETURN_FAIL;
        break;
    }

    STATE = STATE_STRING;

    return STATE_RETURN_CONT;
}

static int state_number(cJSON *item, char byte)
{
    if (byte == '0' || byte == '1' || byte == '2' || byte == '3' || byte == '4' || byte == '5' || byte == '6' ||
        byte == '7' || byte == '8' || byte == '9' || byte == '.' || byte == 'e' || byte == 'E' || byte == '-' ||
        byte == '+')
    {
        scratchbuf[scratch_it++] = byte;
    }
    else if (is_whitespace(byte) || byte == ',' || byte == '}' || byte == ']')
    {
        scratchbuf[scratch_it] = '\0';
        item->valuedouble = strtod(scratchbuf, NULL);
        cJSON_SetNumberValue(item, item->valuedouble);
        scratch_it = 0;
        return STATE_RETURN_DONE;
    }
    else
    {
        return STATE_RETURN_FAIL;
    }

    return STATE_RETURN_CONT;
}

static int state_true(cJSON *item, char byte)
{
    scratchbuf[scratch_it++] = byte;

    if (scratch_it < 4)
    {
        return STATE_RETURN_CONT;
    }

    if (strncmp(scratchbuf, "true", 4) == 0)
    {
        scratch_it = 0;
        return STATE_RETURN_DONE;
    }

    return STATE_RETURN_FAIL;
}

static int state_false(cJSON *item, char byte)
{
    scratchbuf[scratch_it++] = byte;

    if (scratch_it < 5)
    {
        return STATE_RETURN_CONT;
    }

    if (strncmp(scratchbuf, "false", 5) == 0)
    {
        scratch_it = 0;
        return STATE_RETURN_DONE;
    }

    return STATE_RETURN_FAIL;
}

static int state_null(cJSON *item, char byte)
{
    scratchbuf[scratch_it++] = byte;

    if (scratch_it < 4)
    {
        return STATE_RETURN_CONT;
    }

    if (strncmp(scratchbuf, "null", 4) == 0)
    {
        scratch_it = 0;
        return STATE_RETURN_DONE;
    }

    return STATE_RETURN_FAIL;
}

static int putbyte(cJSON *const item, char byte)
{
    switch (STATE)
    {
    case STATE_ITEM:
        return state_item(item, byte);
        break;

    case STATE_OBJECT_KEY:
        return state_object_key(item, byte);
        break;

    case STATE_OBJECT_KEY_PARSED:
        return state_object_key_parsed(item, byte);
        break;

    case STATE_OBJECT_VALUE:
    case STATE_ARRAY_VALUE:
        return state_object_array_value(item, byte);
        break;

    case STATE_OBJECT_VALUE_PARSED:
    case STATE_ARRAY_VALUE_PARSED:
        return state_object_array_value_parsed(item, byte);
        break;

    case STATE_STRING:
        return state_string(item, byte);
        break;

    case STATE_SPECIAL_CHAR:
        return state_special_char(item, byte);
        break;

    case STATE_NUMBER:
        return state_number(item, byte);
        break;

    case STATE_TRUE:
        return state_true(item, byte);
        break;

    case STATE_FALSE:
        return state_false(item, byte);
        break;

    case STATE_NULL:
        return state_null(item, byte);
        break;

    default:
        return STATE_RETURN_FAIL;
        break;
    }
}
