/*
 * Byte-by-byte parser using a finite state machine
 * Copyright (c) 2024 Marc Delling
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include "cJSON.h"

enum {
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

enum {
    STATE_RETURN_FAIL = -1,
    STATE_RETURN_CONT,
    STATE_RETURN_DONE
};

static int putbyte(cJSON * const item, char byte);

// FIXME: duplicated cJSON_New_Item because of visibility
static cJSON *cJSON_New_Item_2()
{
    cJSON* node = (cJSON*)malloc(sizeof(cJSON));
    if (node)
    {
        memset(node, '\0', sizeof(cJSON));
    }
    return node;
}

CJSON_PUBLIC(cJSON *) cJSON_Put(cJSON * item, const char byte, bool * complete)
{
    int retval;

    if (item == NULL)
    {
        item = cJSON_New_Item_2();
    }

    if (item == NULL)
    {
        goto fail;
    }

    retval = putbyte(item, byte);

    if (retval == STATE_RETURN_FAIL)
    {
        goto fail;
    }
    else if (retval == STATE_RETURN_DONE)
    {
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

static void append(cJSON *json, char byte)
{
    // Deliberate misuse of valueint to temporarily keep track of valuestring's length.
    // After parsing is done, valueint is set back to zero.
    // Another approach would be to call strlen() every time, as valuestring is null terminated.
    json->valuestring = realloc(json->valuestring, json->valueint + 1);
    json->valuestring[json->valueint++] = byte;
}

static void invalidate(cJSON *json)
{
    if (json->valuestring)
    {
        free(json->valuestring);
        json->valuestring = NULL;
    }
    json->valueint = 0;
}

static int is_whitespace(char byte)
{
    return (byte <= 0x20);
}

static int state_object_key(cJSON *json, char byte)
{
    int retval;

    if (is_whitespace(byte))
        return STATE_RETURN_CONT;

    if (byte == '}')
        return STATE_RETURN_DONE;

    if (!json->child)
    {
        json->child = cJSON_New_Item_2();
    }

    retval = putbyte(json->child, byte);

    if (retval == STATE_RETURN_DONE)
    {
        json->state = STATE_OBJECT_KEY_PARSED;
        json->child->string = json->child->valuestring;
        json->child->valuestring = NULL;
        json->child->valueint = 0;
        json->child->state = STATE_ITEM;
        return STATE_RETURN_CONT;
    }

    return retval;
}

static int state_object_key_parsed(cJSON *json, char byte)
{
    if (is_whitespace(byte))
    {
        return STATE_RETURN_CONT;
    }

    if (byte == ':')
    {
        json->state = STATE_OBJECT_VALUE;
        return STATE_RETURN_CONT;
    }
    else
    {
        return STATE_RETURN_FAIL;
    }
}

static int state_object_array_value(cJSON *json, char byte)
{
    int retval;

    if (is_whitespace(byte))
    {
        return STATE_RETURN_CONT;
    }

    // FIXME: does not parse empty array '[]'
    //if(byte == ']' && json->state == STATE_ARRAY_VALUE)
    //    return STATE_RETURN_CONT;

    if (!json->child)
    {
        json->child = cJSON_New_Item_2();
    }

    retval = putbyte(json->child, byte);

    if (retval == STATE_RETURN_DONE)
    {
        json->state = (json->state == STATE_ARRAY_VALUE) ? STATE_ARRAY_VALUE_PARSED : STATE_OBJECT_VALUE_PARSED;
        // state_number already consumed the terminating char, so put it back again
        if (json->child->type == cJSON_Number)
        {
            return putbyte(json, byte);
        }
        else
        {
            return STATE_RETURN_CONT;
        }
    }

    return retval;
}

static int state_object_array_value_parsed(cJSON *json, char byte)
{
    if (is_whitespace(byte))
    {
        return STATE_RETURN_CONT;
    }
    else if (byte == ',')
    {
        json->child->next = cJSON_New_Item_2();
        json->child->next->prev = json->child;
        json->child = json->child->next;
        json->state = (json->state == STATE_ARRAY_VALUE_PARSED) ? STATE_ARRAY_VALUE : STATE_OBJECT_KEY;

        return STATE_RETURN_CONT;
    }
    else if ((byte == ']' && json->state == STATE_ARRAY_VALUE_PARSED) || (byte == '}' && json->state == STATE_OBJECT_VALUE_PARSED))
    {
        while(json->child->prev)
        {
            json->child = json->child->prev;
        }
        return STATE_RETURN_DONE;
    }
    else
    {
        return STATE_RETURN_FAIL;
    }
}

static int state_item(cJSON *json, char byte)
{
    switch (byte)
    {
    case '{':
        json->state = STATE_OBJECT_KEY;
        json->type = cJSON_Object;
        return STATE_RETURN_CONT;
    case '[':
        json->state = STATE_ARRAY_VALUE;
        json->type = cJSON_Array;
        return STATE_RETURN_CONT;
    case '"':
        json->state = STATE_STRING;
        json->type = cJSON_String;
        return STATE_RETURN_CONT;
    case 't':
        json->state = STATE_TRUE;
        json->type = cJSON_True;
        append(json, byte);
        return STATE_RETURN_CONT;
    case 'f':
        json->state = STATE_FALSE;
        json->type = cJSON_False;
        append(json, byte);
        return STATE_RETURN_CONT;
    case 'n':
        json->state = STATE_NULL;
        json->type = cJSON_NULL;
        append(json, byte);
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
        json->state = STATE_NUMBER;
        json->type = cJSON_Number;
        append(json, byte);
        return STATE_RETURN_CONT;
    default:
        return STATE_RETURN_FAIL;
    }
}

static int state_string(cJSON *json, char byte)
{
    if (byte == '"')
    {
        append(json, '\0');
        return STATE_RETURN_DONE;
    }
    else if (byte == '\\')
    {
        json->state = STATE_SPECIAL_CHAR;
    }
    else
    {
        append(json, byte);
    }

    return STATE_RETURN_CONT;
}

static int state_special_char(cJSON *json, char byte)
{
    switch (byte)
    {
    case 'b':
        append(json, '\b');
        break;

    case 'f':
        append(json, '\f');
        break;

    case 'n':
        append(json, '\n');
        break;

    case 'r':
        append(json, '\r');
        break;

    case 't':
        append(json, '\t');
        break;

    case '"':
    case '\\':
    case '/':
        append(json, byte);
        break;

    default:
        return STATE_RETURN_FAIL;
        break;
    }

    json->state = STATE_STRING;

    return STATE_RETURN_CONT;
}

static int state_number(cJSON *json, char byte)
{
    if (byte == '0' || byte == '1' || byte == '2' || byte == '3' || byte == '4' || byte == '5' || byte == '6' ||
        byte == '7' || byte == '8' || byte == '9' || byte == '.' || byte == 'e' || byte == 'E' || byte == '-' || byte == '+')
    {
        append(json, byte);
    }
    else if (is_whitespace(byte) || byte == ',' || byte == '}' || byte == ']')
    {
        append(json, '\0');
        json->valuedouble = strtod(json->valuestring, 0);
        invalidate(json);
        return STATE_RETURN_DONE;
    }
    else
    {
        return STATE_RETURN_FAIL;
    }

    return STATE_RETURN_CONT;
}

static int state_true(cJSON *json, char byte)
{
    append(json, byte);

    if (json->valueint < 4)
    {
        return STATE_RETURN_CONT;
    }

    if (strncmp(json->valuestring, "true", 4) == 0)
    {
        invalidate(json);
        return STATE_RETURN_DONE;
    }

    return STATE_RETURN_FAIL;
}

static int state_false(cJSON *json, char byte)
{
    append(json, byte);

    if (json->valueint < 5)
    {
        return STATE_RETURN_CONT;
    }

    if (strncmp(json->valuestring, "false", 5) == 0)
    {
        invalidate(json);
        return STATE_RETURN_DONE;
    }

    return STATE_RETURN_FAIL;
}

static int state_null(cJSON *json, char byte)
{
    append(json, byte);

    if (json->valueint < 4)
    {
        return STATE_RETURN_CONT;
    }

    if (strncmp(json->valuestring, "null", 4) != 0)
    {
        return STATE_RETURN_FAIL;
    }

    invalidate(json);

    return STATE_RETURN_DONE;
}

static int putbyte(cJSON * const item, char byte)
{
    switch (item->state)
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
