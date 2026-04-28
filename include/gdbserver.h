/*
 *  GDB remote serial protocol stub for DOSBox.
 *
 *  This file is part of DOSBox.  See COPYING for license terms.
 */

#ifndef DOSBOX_GDBSERVER_H
#define DOSBOX_GDBSERVER_H

#include "dosbox.h"

#if C_GDBSERVER

class Section;

void GDB_Init(Section* sec);
void GDB_Shutdown(Section* sec);

bool GDB_HeavyCheck(void);

void GDB_RequestBreak(void);

#endif /* C_GDBSERVER */

#endif
