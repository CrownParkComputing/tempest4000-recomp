/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pad mapping: the keyboard layout and the gamepad's Back button. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/runtime_pad.c"

static int menu_open, other_overlay, menu_toggles;
int tempest_gpu_overlay_captures_input(void) { return menu_open || other_overlay; }
int tempest_skip_intro(void) { return 0; }
int tempest_display_settings_is_open(void) { return menu_open; }
void tempest_display_settings_open(void) { menu_open=1; }
void tempest_display_settings_toggle(void) { menu_open=!menu_open; ++menu_toggles; }
int tempest_title_back_exits(void) { return 1; }
void tempest_exit_prompt_open(void) {}
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static bool keys[SDL_SCANCODE_COUNT];

/* The keyboard's contribution to a neutral pad with the keys in `held` down. */
static PadData press(const SDL_Scancode *held, size_t count) {
    memset(keys,0,sizeof(keys));
    for (size_t i=0;i<count;++i) keys[held[i]]=true;
    PadData d;
    memset(&d,0,sizeof(d));
    d.left_x=d.left_y=d.right_x=d.right_y=128;
    sample_keyboard(&d,keys);
    return d;
}
static PadData press1(SDL_Scancode key) { return press(&key,1); }

static size_t sources_of(const ButtonMap *map, uint32_t ps) {
    size_t n=0;
    for (size_t i=0;i<map->count;++i) n+=map->entries[i].ps==ps;
    return n;
}

static void test_default_keyboard(void) {
    assert(press1(SDL_SCANCODE_SPACE).buttons==BTN_CROSS);
    assert(press1(SDL_SCANCODE_RETURN).buttons==BTN_CROSS);
    assert(press1(SDL_SCANCODE_ESCAPE).buttons==BTN_CIRCLE);
    assert(press1(SDL_SCANCODE_F).buttons==BTN_SQUARE);
    assert(press1(SDL_SCANCODE_H).buttons==BTN_TRIANGLE);
    assert(press1(SDL_SCANCODE_Q).buttons==BTN_L1 && press1(SDL_SCANCODE_O).buttons==BTN_R1);
    assert(press1(SDL_SCANCODE_UP).buttons==BTN_UP && press1(SDL_SCANCODE_RIGHT).buttons==BTN_RIGHT);
    assert(press1(SDL_SCANCODE_TAB).buttons==BTN_OPTIONS);
    assert(press1(SDL_SCANCODE_T).buttons==CLICK); /* play_click turns it into a touch */
    PadData d=press1(SDL_SCANCODE_E);
    assert(d.buttons==BTN_L2 && d.l2==255);
    /* Sticks: no button bits, full tilt, both sticks at once. */
    const SDL_Scancode slide[]={SDL_SCANCODE_W,SDL_SCANCODE_D,SDL_SCANCODE_J};
    d=press(slide,3);
    assert(d.buttons==0);
    assert(d.left_y==0 && d.left_x==255 && d.right_x==0 && d.right_y==128);
    /* Two buttons at once, one from each hand. */
    const SDL_Scancode taps[]={SDL_SCANCODE_F,SDL_SCANCODE_H};
    assert(press(taps,2).buttons==(BTN_SQUARE|BTN_TRIANGLE));
    assert(press(NULL,0).buttons==0);
}

static void test_pad_buttons(void) {
    assert(sources_of(&pad_map,CLICK)==1); /* Back/View clicks the touchpad */
    assert(sources_of(&pad_map,BTN_CROSS)==1 && sources_of(&key_map,BTN_CROSS)==3);
}

static void test_menu_shortcut(void) {
    MenuShortcut state={0};
    const uint32_t clicks=BTN_L3|BTN_R3;
    PadData d={.buttons=BTN_L3|BTN_CROSS};
    menu_shortcut_sample(&state,&d,true,false,clicks);
    assert(d.buttons==(BTN_L3|BTN_CROSS) && !menu_toggles);
    d.buttons=clicks|BTN_CROSS;
    menu_shortcut_sample(&state,&d,true,true,clicks);
    assert(menu_open && menu_toggles==1 && d.buttons==BTN_CROSS);
    for (int i=0;i<10;++i) {
        d.buttons=clicks;
        menu_shortcut_sample(&state,&d,true,true,clicks);
        assert(menu_open && menu_toggles==1 && !d.buttons);
    }
    /* Closing another way while held cannot reopen the menu. */
    menu_open=0; d.buttons=BTN_R3;
    menu_shortcut_sample(&state,&d,false,true,clicks);
    assert(!d.buttons && menu_toggles==1);
    menu_shortcut_sample(&state,&d,true,true,clicks);
    assert(!menu_open && menu_toggles==1);
    menu_shortcut_sample(&state,&d,false,false,clicks);
    menu_shortcut_sample(&state,&d,true,true,clicks);
    assert(menu_open && menu_toggles==2);
    menu_shortcut_sample(&state,&d,false,false,clicks);
    menu_shortcut_sample(&state,&d,true,true,clicks);
    assert(!menu_open && menu_toggles==3);
    /* A save/text dialog cannot be displaced by the menu chord. */
    other_overlay=1;
    menu_shortcut_sample(&state,&d,false,false,clicks);
    menu_shortcut_sample(&state,&d,true,true,clicks);
    assert(!menu_open && menu_toggles==3);
    other_overlay=0;
    menu_shortcut_sample(&state,&d,true,true,clicks);
    assert(!menu_open && menu_toggles==3);
    /* Consume the physical clicks even with custom button mappings. */
    menu_shortcut_sample(&state,&d,false,false,clicks);
    d.buttons=BTN_CROSS|BTN_TRIANGLE|BTN_L1;
    menu_shortcut_sample(&state,&d,true,true,clicks|BTN_CROSS|BTN_TRIANGLE);
    assert(menu_open && menu_toggles==4 && d.buttons==BTN_L1);
    menu_open=0;
}

int main(void) {
    test_default_keyboard();
    test_pad_buttons();
    test_menu_shortcut();
    puts("pad tests passed");
    return 0;
}
