// cmdbox_labels.h — see cmdbox_labels.cpp. Shifts the field command-menu label
// text + "Command" header graphic left (data patch on the 0x95BA7C position
// table). Config: cmdbox_label_x / cmdbox_cmdgfx_x (0 = off). Gated + revertable.
#pragma once

void cmdbox_labels_install();
