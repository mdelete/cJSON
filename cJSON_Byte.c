/*
 * Byte-by-byte parser using a finite state machine
 * Copyright (c) 2024 Marc Delling
 */

#include <stdbool.h>
#include <stdio.h>
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

static char scratchbuf[128]; // This makes it thread-unsafe but massively
                             // reduces allocations

// FIXME: duplicated cJSON_New_Item because of visibility
static cJSON *cJSON_New_Item_2()
{
    cJSON *node = (cJSON *)malloc(sizeof(cJSON));
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
        printf("parse fail at '%c'", byte);
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

static int append(cJSON *item, char byte)
{
    // Deliberate misuse of valueint to temporarily keep track of valuestring's
    // length. After parsing is done, valueint is set back to zero. Another
    // approach would be to call strlen() every time, as valuestring is null
    // terminated.
    if (item->valueint >= sizeof(scratchbuf))
    {
        return STATE_RETURN_FAIL;
    }

    scratchbuf[item->valueint] = byte;

    if (byte == '\0')
    {
        item->valuestring = malloc(item->valueint);
        if (item->valuestring == NULL)
        {
            return STATE_RETURN_FAIL;
        }
        memcpy(item->valuestring, scratchbuf, item->valueint);
        item->valueint = 0;

        return STATE_RETURN_DONE;
    }

    item->valueint++;

    return STATE_RETURN_CONT;
}

static int is_whitespace(char byte)
{
    return (byte <= 0x20);
}

static int state_object_key(cJSON *item, char byte)
{
    int retval;

    if (is_whitespace(byte))
        return STATE_RETURN_CONT;

    if (byte == '}')
        return STATE_RETURN_DONE;

    if (item->child == NULL)
    {
        item->child = cJSON_New_Item_2();

        if (item->child == NULL)
        {
            return STATE_RETURN_FAIL;
        }
    }

    retval = putbyte(item->child, byte);

    if (retval == STATE_RETURN_DONE)
    {
        item->state = STATE_OBJECT_KEY_PARSED;
        item->child->string = item->child->valuestring;
        item->child->valuestring = NULL;
        item->child->valueint = 0;
        item->child->state = STATE_ITEM;
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

    if (byte == ':')
    {
        item->state = STATE_OBJECT_VALUE;
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

    if (is_whitespace(byte))
    {
        return STATE_RETURN_CONT;
    }

    // FIXME: does not parse empty array '[]'
    // if(byte == ']' && json->state == STATE_ARRAY_VALUE)
    //    return STATE_RETURN_CONT;

    if (item->child == NULL)
    {
        item->child = cJSON_New_Item_2();

        if (item->child == NULL)
        {
            return STATE_RETURN_FAIL;
        }
    }

    retval = putbyte(item->child, byte);

    if (retval == STATE_RETURN_DONE)
    {
        item->state = (item->state == STATE_ARRAY_VALUE) ? STATE_ARRAY_VALUE_PARSED : STATE_OBJECT_VALUE_PARSED;
        // state_number already consumed the terminating char, so put it back
        // again
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
        item->state = (item->state == STATE_ARRAY_VALUE_PARSED) ? STATE_ARRAY_VALUE : STATE_OBJECT_KEY;

        return STATE_RETURN_CONT;
    }
    else if ((byte == ']' && item->state == STATE_ARRAY_VALUE_PARSED) ||
             (byte == '}' && item->state == STATE_OBJECT_VALUE_PARSED))
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
        item->state = STATE_OBJECT_KEY;
        item->type = cJSON_Object;
        return STATE_RETURN_CONT;
    case '[':
        item->state = STATE_ARRAY_VALUE;
        item->type = cJSON_Array;
        return STATE_RETURN_CONT;
    case '"':
        item->state = STATE_STRING;
        item->type = cJSON_String;
        return STATE_RETURN_CONT;
    case 't':
        item->state = STATE_TRUE;
        item->type = cJSON_True;
        scratchbuf[item->valueint++] = byte;
        return STATE_RETURN_CONT;
    case 'f':
        item->state = STATE_FALSE;
        item->type = cJSON_False;
        scratchbuf[item->valueint++] = byte;
        return STATE_RETURN_CONT;
    case 'n':
        item->state = STATE_NULL;
        item->type = cJSON_NULL;
        scratchbuf[item->valueint++] = byte;
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
        item->state = STATE_NUMBER;
        item->type = cJSON_Number;
        scratchbuf[item->valueint++] = byte;
        return STATE_RETURN_CONT;
    default:
        return STATE_RETURN_FAIL;
    }
}

static int state_string(cJSON *item, char byte)
{
    if (byte == '"')
    {
        append(item, '\0');
        return STATE_RETURN_DONE;
    }
    else if (byte == '\\')
    {
        item->state = STATE_SPECIAL_CHAR;
    }
    else
    {
        append(item, byte);
    }

    return STATE_RETURN_CONT;
}

static int state_special_char(cJSON *item, char byte)
{
    switch (byte)
    {
    case 'b':
        append(item, '\b');
        break;

    case 'f':
        append(item, '\f');
        break;

    case 'n':
        append(item, '\n');
        break;

    case 'r':
        append(item, '\r');
        break;

    case 't':
        append(item, '\t');
        break;

    case '"':
    case '\\':
    case '/':
        append(item, byte);
        break;

    default:
        return STATE_RETURN_FAIL;
        break;
    }

    item->state = STATE_STRING;

    return STATE_RETURN_CONT;
}

static int state_number(cJSON *item, char byte)
{
    if (byte == '0' || byte == '1' || byte == '2' || byte == '3' || byte == '4' || byte == '5' || byte == '6' ||
        byte == '7' || byte == '8' || byte == '9' || byte == '.' || byte == 'e' || byte == 'E' || byte == '-' ||
        byte == '+')
    {
        scratchbuf[item->valueint++] = byte;
    }
    else if (is_whitespace(byte) || byte == ',' || byte == '}' || byte == ']')
    {
        scratchbuf[item->valueint] = '\0';
        item->valuedouble = strtod(scratchbuf, 0);
        item->valueint = 0;
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
    scratchbuf[item->valueint++] = byte;

    if (item->valueint < 4)
    {
        return STATE_RETURN_CONT;
    }

    if (strncmp(scratchbuf, "true", 4) == 0)
    {
        item->valueint = 0;
        return STATE_RETURN_DONE;
    }

    return STATE_RETURN_FAIL;
}

static int state_false(cJSON *item, char byte)
{
    scratchbuf[item->valueint++] = byte;

    if (item->valueint < 5)
    {
        return STATE_RETURN_CONT;
    }

    if (strncmp(scratchbuf, "false", 5) == 0)
    {
        item->valueint = 0;
        return STATE_RETURN_DONE;
    }

    return STATE_RETURN_FAIL;
}

static int state_null(cJSON *item, char byte)
{
    scratchbuf[item->valueint++] = byte;

    if (item->valueint < 4)
    {
        return STATE_RETURN_CONT;
    }

    if (strncmp(scratchbuf, "null", 4) != 0)
    {
        return STATE_RETURN_FAIL;
    }

    item->valueint = 0;

    return STATE_RETURN_DONE;
}

static int putbyte(cJSON *const item, char byte)
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
