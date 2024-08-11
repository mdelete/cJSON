/*
 * gcc -o test2 test2.c cJSON.c cJSON_Byte.c
 */

#include <stdio.h>
#include <stdbool.h>

#include "cJSON.h"

CJSON_PUBLIC(cJSON *) cJSON_Put(cJSON * item, const char byte, bool * complete);

int main()
{
    char ch;
    cJSON *item = NULL;
    bool complete;

    while ((ch = getchar())) {
        item = cJSON_Put(item, ch, &complete);
        if (item && complete) {
            printf("cJSON_Print: %s\n", cJSON_Print(item));
            cJSON_Delete(item);
            item = NULL;
            complete = false;
        }
    }

    return 0;
}
