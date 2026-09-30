/* SPDX-License-Identifier: MIT */
#ifndef HOGTIED_UI_H
#define HOGTIED_UI_H

#include "hbas/vehicle.h"

#define UI_WIDTH  400     /* premium panel, CROSS_CHECKS.md sec. 7 */
#define UI_HEIGHT 240

/* Navigation keys. Handlebar/faceplate mapping comes with the IOC driver. */
enum ui_key { UI_KEY_LEFT, UI_KEY_RIGHT, UI_KEY_UP, UI_KEY_DOWN, UI_KEY_ENTER, UI_KEY_BACK };

enum ui_page { UI_PAGE_DASH, UI_PAGE_TIRES, UI_PAGE_SYSTEM, UI_PAGE_COUNT };

void ui_create(void);
void ui_update(const struct hbas_vehicle *v);
void ui_key(enum ui_key key);
enum ui_page ui_current_page(void);

#endif
