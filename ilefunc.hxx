/*
 * Copyright (c) 2025 Seiden Group
 *
 * SPDX-License-Identifier: ISC
 */

extern "C" {
	#include <as400_protos.h>
	#include <unistd.h>
}

#include <array>
#include <cstdlib>
#include <string>

/*
 * Type-safe compile-time wrapper for IBM i ILE service programs. ILE passes
 * arguments by value; the PASE interface requires them to be passed through
 * memory. PASE pointers are automatically converted into ILEpointer structs
 * with correct alignment.
 *
 * Thanks to Saagar Jha for help with the building the arglist.
 */

// This isn't part of ILEArgument because void specialization breaking write()
template<typename T> constexpr result_type_t ResultReturnType() {
	return sizeof(T);
};
template<>constexpr result_type_t ResultReturnType<void>(){return RESULT_VOID;}
template<>constexpr result_type_t ResultReturnType<int8_t>(){return RESULT_INT8;}
template<>constexpr result_type_t ResultReturnType<uint8_t>(){return RESULT_UINT8;}
template<>constexpr result_type_t ResultReturnType<int16_t>(){return RESULT_INT16;}
template<>constexpr result_type_t ResultReturnType<uint16_t>(){return RESULT_UINT16;}
template<>constexpr result_type_t ResultReturnType<int32_t>(){return RESULT_INT32;}
template<>constexpr result_type_t ResultReturnType<uint32_t>(){return RESULT_UINT32;}
template<>constexpr result_type_t ResultReturnType<int64_t>(){return RESULT_INT64;}
template<>constexpr result_type_t ResultReturnType<uint64_t>(){return RESULT_UINT64;}
template<>constexpr result_type_t ResultReturnType<double>(){return RESULT_FLOAT64;}

// XXX: Handle aggregate case
template<typename T> T BaseReturn(ILEarglist_base *base);
#define DefineBaseReturnSubst(T, accessor) \
	template<> T BaseReturn<T>(ILEarglist_base *base){return base->result. accessor;}
template<> void BaseReturn<void>(ILEarglist_base*){return;}
DefineBaseReturnSubst(int8_t, s_int8.r_int8);
DefineBaseReturnSubst(uint8_t, s_uint8.r_uint8);
DefineBaseReturnSubst(int16_t, s_int16.r_int16);
DefineBaseReturnSubst(uint16_t, s_uint16.r_uint16);
DefineBaseReturnSubst(int32_t, s_int32.r_int32);
DefineBaseReturnSubst(uint32_t, s_uint32.r_uint32);
DefineBaseReturnSubst(int64_t, r_int64);
DefineBaseReturnSubst(uint64_t, r_uint64);

template<typename T>
class ILEArgument {
public:
	ILEArgument() = delete;

	static constexpr size_t align() {
		return alignof(T);
	}

	static constexpr arg_type_t type() {
		return sizeof(T);
	}

	// This is an overload on ILEArgument instead of in ILEArglist to
	// work around an issue with GCC pre-10 confusing T and T*.
	static inline void write(char *dst, T src) {
		// XXX: Should this be with tags?
                memcpy(dst, &src, sizeof(src));
        }
};

template<>constexpr arg_type_t ILEArgument<int8_t>::type(){return ARG_INT8;}
template<>constexpr arg_type_t ILEArgument<uint8_t>::type(){return ARG_UINT8;}
template<>constexpr arg_type_t ILEArgument<int16_t>::type(){return ARG_INT16;}
template<>constexpr arg_type_t ILEArgument<uint16_t>::type(){return ARG_UINT16;}
template<>constexpr arg_type_t ILEArgument<int32_t>::type(){return ARG_INT32;}
template<>constexpr arg_type_t ILEArgument<uint32_t>::type(){return ARG_UINT32;}
template<>constexpr arg_type_t ILEArgument<int64_t>::type(){return ARG_INT64;}
template<>constexpr arg_type_t ILEArgument<uint64_t>::type(){return ARG_UINT64;}
template<>constexpr arg_type_t ILEArgument<float>::type(){return ARG_FLOAT32;}
template<>constexpr arg_type_t ILEArgument<double>::type(){return ARG_FLOAT64;}
// XXX: Teraspace, space, open pointers

template<typename T>
class ILEArgument<T*> {
public:
	static constexpr size_t align() {
		// Use sizeof in case we're using older IBM i headers
		// without the correct alignment attributes for GCC.
		return sizeof(ILEpointer);
	}

	static constexpr arg_type_t type() {
		return ARG_MEMPTR;
	}

	static inline void write(char *dst, T *src) {
		// The high word should be unset as it's initialized in ctor
                ILEpointer *p = (ILEpointer*)dst;
                p->s.addr = (address64_t)src;
        }
};

template <typename... TArgs>
class ILEArglist {
	using Sizes = std::array<std::size_t, sizeof...(TArgs)>;
public:
	ILEArglist(TArgs... args) {
		this->base = {};
		this->arguments = {};
		write<0>(args...);
	}

	static constexpr auto offsets() {
		auto sizes = Sizes{ILEArgument<TArgs>::align()...};
		auto offsets = Sizes();
		// const& is required for [] and back to be constexpr in C++14
		const Sizes &sizes_c = sizes;
		const Sizes &offsets_c = offsets;
		// Like an exclusive scan, but align to the preferred alignment of the type
		for (size_t i = 0; i < sizes.size(); i++) {
			if (i == 0) {
				continue;
			}
			const size_t base = (sizes_c[i - 1] + offsets_c[i - 1]);
			const size_t aligned = (base + sizes_c[i] - 1) & -(sizes_c[i]);
			const_cast<typename Sizes::reference>(offsets_c[i]) = aligned;
		}
		return offsets;
	}

	static constexpr auto size() {
		if (sizeof...(TArgs) == 0) {
			return (size_t)0;
		}
		auto sizes = Sizes{ILEArgument<TArgs>::align()...};
		auto offsets = ILEArglist::offsets();
		const Sizes &sizes_c = sizes;
		const Sizes &offsets_c = offsets;
		return sizes_c.back() + offsets_c.back();
	}

	__attribute__((aligned(16))) ILEarglist_base base;
	__attribute__((aligned(16))) std::array<char, size()> arguments;
private:
	template <size_t index>
	void write() {
	}

	template <size_t index, typename T, typename... Ts>
	void write(T argument, Ts... rest) {
		ILEArgument<T>::write(this->arguments.data() + offsets()[index], argument);
		write<index + 1>(rest...);
	}
};

template<typename TReturn, typename... TArgs>
class ILEFunction {
	using ActivationMark = unsigned long long;
public:
	ILEFunction(const char *path, const char *symbol, int flags = 0) {
		this->my_pid = -1;
		this->activation_mark = -1;
		this->procedure = {};
		// Invalid flags will result in... ILECALL_INVALID_FLAGS
		this->flags = flags & (ILECALL_NOINTERRUPT | ILECALL_EXCP_NOSIGNAL);
		this->path = path;
		this->symbol = symbol;
		this->signature = {
			ILEArgument<TArgs>::type()..., ARG_END
		};
	}

	bool init() {
		// Forking will destroy the activation mark
		pid_t current_pid = getpid();
		bool pid_matches = this->my_pid == current_pid;
		if (pid_matches) {
			return true;
		}
		this->activation_mark = _ILELOADX(this->path.c_str(), ILELOAD_LIBOBJ);
		// XXX: Should distinguish failure types
		if (this->activation_mark == (ActivationMark)-1) {
			return false;
		}
		if (_ILESYMX(&this->procedure, this->activation_mark, this->symbol.c_str()) != ILESYM_PROCEDURE) {
			return false;
		}
		this->my_pid = current_pid;
		return true;
	}

	TReturn operator()(TArgs... args) {
		// XXX: Throw?
		this->init();
		// can just be ILEArglist(args...) in C++17
		auto arguments = ILEArglist<TArgs...>(args...);
		int rc = _ILECALLX(&this->procedure, &arguments.base, this->signature.data(), ResultReturnType<TReturn>(), this->flags);
		// 0 is OK, -1 seems to be an MI exception, positive is _ILECALL error
		if (rc > 0) {
			abort();
		}
		// XXX: Tagged pointers, aggregates
		return BaseReturn<TReturn>(&arguments.base);
	}
private:
	ActivationMark activation_mark;
	pid_t my_pid;
	int flags;
	ILEpointer procedure __attribute__ ((aligned (16)));
	std::string path, symbol;
	std::array<arg_type_t, sizeof...(TArgs) + 1> signature;
};
