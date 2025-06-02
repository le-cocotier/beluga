#ifndef INIT_H_
#define INIT_H_

#include "data.h"
#include "terminal.h"
#include "builtins.h"
#include "../blisp/include/config_tools.h"
#include <stdio.h>


void getConfig();

/**
 * \fn void initEditor()
 * \brief Job's function is to initialize all the fields of editorConfig.
 * */

void initEditor(struct editorConfig *E);

#endif // INIT_H_
