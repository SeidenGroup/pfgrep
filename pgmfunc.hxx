/*
 * Copyright (c) 2025 Seiden Group
 *
 * SPDX-License-Identifier: ISC
 */

extern "C" {
	#include <as400_protos.h>
}

/*
 * Type-safe compile-time wrapper for IBM i *PGM objects. The OPM calling
 * convention is pass-by-reference; this automatically passes value types
 * passed to the function into pointers to the stack.
 */

// If passed by value: get the reference on the stack (and de-constify)
template<typename T> constexpr void *ParameterArrayMember(T &obj) { return static_cast<void*>(&obj); }
template<typename T> constexpr void *ParameterArrayMember(const T &obj) { return ParameterArrayMember(const_cast<T&>(obj)); }
// If it's already a pointer, just pass it through
template<typename T> constexpr void *ParameterArrayMember(T *obj) { return static_cast<void*>(obj); }
template<typename T> constexpr void *ParameterArrayMember(const T *obj) { return ParameterArrayMember(const_cast<T*>(obj)); }

template<typename... TArgs>
class PGMFunction {
public:
	PGMFunction(const char *library, const char *object, int flags = 0) {
		static_assert(sizeof...(TArgs) <= 16383, "_PGMCALL maximum arguments reached");

		this->flags = process_flags(flags);
		// Unlike ILE handles, these survive forks
		if (_RSLOBJ2(&this->pgm, RSLOBJ_TS_PGM, object, library)) {
			// XXX: Throw?
		}
	}
	int operator ()(TArgs... args) {
		void *pgm_argv[] = {ParameterArrayMember(args)..., NULL};
		return _PGMCALL(&this->pgm, pgm_argv, this->flags);
	}
private:
	constexpr int process_flags(int flags) {
		if (sizeof...(TArgs) > PGMCALL_MAXARGS) {
			flags |= PGMCALL_NOMAXARGS;
		}
		// XXX: PGMCALL_DIRECT_ARGS requires different argv type
		return flags & (PGMCALL_DROP_ADOPT
			| PGMCALL_NOINTERRUPT
			| PGMCALL_ASCII_STRINGS
			| PGMCALL_EXCP_NOSIGNAL);
	}

	ILEpointer pgm __attribute__ ((aligned (16)));
	int flags;
};
