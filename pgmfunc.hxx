/*
 * Copyright (c) 2025 Seiden Group
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
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
template<typename T> constexpr void *ParameterArrayMember(T &obj) { return (void*)&obj; }
// If it's already a pointer, just pass it through
template<typename T> constexpr void *ParameterArrayMember(T *obj) { return (void*)obj; }

template<typename... TArgs>
class PGMFunction {
public:
	PGMFunction(const char *library, const char *object) {
		// Unlike ILE handles, these survive forks
		if (_RSLOBJ2(&this->pgm, RSLOBJ_TS_PGM, object, library)) {
			// XXX: Throw?
		}
	}
	int operator ()(TArgs... args) {
		void *pgm_argv[] = {ParameterArrayMember(args)..., NULL};
		return _PGMCALL(&this->pgm, pgm_argv, PGMCALL_EXCP_NOSIGNAL);
	}
private:
	ILEpointer pgm __attribute__ ((aligned (16)));
};
