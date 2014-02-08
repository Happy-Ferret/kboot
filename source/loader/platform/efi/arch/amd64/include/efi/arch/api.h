/*
 * Copyright (C) 2014 Alex Smith
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
 * @brief		AMD64 EFI API definitions.
 */

#ifndef __EFI_ARCH_API_H
#define __EFI_ARCH_API_H

/**
 * EFI calling convention attribute.
 *
 * On x86_64, EFI uses the Microsoft calling convention, therefore we must
 * mark all EFI APIs with the ms_abi attribute so that the right calling
 * convention is used.
 */
#define __efiapi	__attribute__((ms_abi))

/**
 * EFI call wrapper.
 *
 * We must wrap EFI calls to restore the firmware's GDT/IDT before calling, and
 * restore ours afterward. This is a slightly nasty hack to call functions via
 * a wrapper (in start.S), that keeps type safety and relies on the compiler to
 * put all arguments in the right place.
 */
#define efi_call(func, args...) \
	__extension__ \
	({ \
		typeof(func) __wrapper = (typeof(func))__efi_call; \
		__efi_call_func = (void *)func; \
		__wrapper(args); \
	})

extern void *__efi_call_func;
extern unsigned long __efi_call(void) __efiapi;

#endif /* __EFI_ARCH_API_H */
