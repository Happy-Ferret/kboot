/*
 * Copyright (C) 2010-2014 Alex Smith
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * @file
 * @brief               Boot error handling functions.
 */

#include <lib/printf.h>

#include <console.h>
#include <loader.h>
#include <memory.h>
#include <shell.h>
#include <ui.h>

/** Boot error message. */
static const char *boot_error_format;
static va_list boot_error_args;

/** Helper for printing error messages.
 * @param ch            Character to display.
 * @param data          Ignored.
 * @param total         Pointer to total character count. */
static void error_printf_helper(char ch, void *data, int *total) {
    console_putc(&debug_console, ch);
    console_putc(&main_console, ch);

    *total = *total + 1;
}

/** Formatted print function for error functions. */
static int error_printf(const char *fmt, ...) {
    va_list args;
    int ret;

    va_start(args, fmt);
    ret = do_vprintf(error_printf_helper, NULL, fmt, args);
    va_end(args);

    return ret;
}

/** Raise an internal error.
 * @param fmt           Error format string.
 * @param ...           Values to substitute into format. */
void __noreturn internal_error(const char *fmt, ...) {
    va_list args;

    if (main_console.out)
        main_console.out->reset(main_console.out_private);

    error_printf("\nInternal Error: ");

    va_start(args, fmt);
    do_vprintf(error_printf_helper, NULL, fmt, args);
    va_end(args);

    error_printf("\n\n");
    error_printf("Please report this error to http://kiwi.alex-smith.me.uk/\n");
    #ifdef __PIC__
        error_printf("Backtrace (base = %p):\n", __start);
    #else
        error_printf("Backtrace:\n");
    #endif
    backtrace(error_printf);

    system_halt();
}

/** Display the boot error message. */
static void boot_error_message(void) {
    do_vprintf(error_printf_helper, NULL, boot_error_format, boot_error_args);

    error_printf("\n\n");
    error_printf("Ensure that you have enough memory available, that you do not have any\n");
    error_printf("malfunctioning hardware and that your computer meets the minimum system\n");
    error_printf("requirements for the operating system.\n");
}

#ifdef CONFIG_TARGET_HAS_UI

/** Render the boot error window.
 * @param Window        Window to render. */
static void boot_error_render(ui_window_t *window) {
    boot_error_message();
}

/** Write the help text for the boot error.
 * @param window        Window to write for. */
static void boot_error_help(ui_window_t *window) {
    ui_print_action('\e', "Reboot");
    ui_print_action(CONSOLE_KEY_F1, "Shell");
    ui_print_action(CONSOLE_KEY_F2, "Debug Log");
}

/** Handle input on the boot error window.
 * @param window        Window input was performed on.
 * @param key           Key that was pressed.
 * @return              Input handling result. */
static input_result_t boot_error_input(ui_window_t *window, uint16_t key) {
    /* TODO: Reboot, debug log. */
    switch (key) {
    case CONSOLE_KEY_F1:
        /* We start the shell in boot_error() upon return. */
        return INPUT_CLOSE;
    default:
        return INPUT_HANDLED;
    }
}

/** Boot error window type. */
static ui_window_type_t boot_error_window_type = {
    .render = boot_error_render,
    .help = boot_error_help,
    .input = boot_error_input,
};

#endif /* CONFIG_TARGET_HAS_UI */

// return for shell
/** Display details of a boot error.
 * @param fmt           Error format string.
 * @param ...           Values to substitute into format. */
void __noreturn boot_error(const char *fmt, ...) {
    console_printf(&debug_console, "\nBoot Error: ");

    /* Save the format string and arguments for UI render code. */
    boot_error_format = fmt;
    va_start(boot_error_args, fmt);

    #ifdef CONFIG_TARGET_HAS_UI
        ui_window_t *window;

        window = malloc(sizeof(*window));
        window->type = &boot_error_window_type;
        window->title = "Boot Error";

        ui_display(window, &main_console, 0);
        ui_window_destroy(window);
    #else
        /* Just print it straight out on the console. */
        console_reset(&main_console);
        console_printf(&main_console, "\nBoot Error: ");
        boot_error_message();
        error_printf("\n");
    #endif

    va_end(boot_error_args);

    /* Jump into the shell. */
    shell_main();
    system_halt();
}