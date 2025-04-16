GC_COLLECTOR ?= pcc
GC_EMBEDDER_H ?= whippet-embedder.h
GC_EMBEDDER_CPPFLAGS ?=
GC_EMBEDDER_CFLAGS ?=

DEFAULT_BUILD := opt

BUILD_CPPFLAGS_opt      = -DNDEBUG
BUILD_CPPFLAGS_optdebug = -DGC_DEBUG=1
BUILD_CPPFLAGS_debug    = -DGC_DEBUG=1

BUILD_CFLAGS_opt      	= -O2 -g
BUILD_CFLAGS_optdebug 	= -O2 -g
BUILD_CFLAGS_debug    	= -O0 -g

GC_BUILD_CPPFLAGS = $(BUILD_CPPFLAGS_$(or $(GC_BUILD),$(DEFAULT_BUILD)))
GC_BUILD_CFLAGS = $(BUILD_CFLAGS_$(or $(GC_BUILD),$(DEFAULT_BUILD)))

V ?= 1
v_0 = @
v_1 =

GC_USE_LTTNG_0 :=
GC_USE_LTTNG_1 := 1
GC_USE_LTTNG := $(shell pkg-config --exists lttng-ust && echo 1 || echo 0)
GC_LTTNG_CPPFLAGS := $(if $(GC_USE_LTTNG_$(GC_USE_LTTNG)), $(shell pkg-config --cflags lttng-ust),)
GC_LTTNG_LIBS := $(if $(GC_USE_LTTNG_$(GC_USE_LTTNG)), $(shell pkg-config --libs lttng-ust),)
GC_TRACEPOINT_CPPFLAGS = $(if $(GC_USE_LTTNG_$(GC_USE_LTTNG)),$(GC_LTTNG_CPPFLAGS) -DGC_TRACEPOINT_LTTNG=1,)
GC_TRACEPOINT_LIBS = $(GC_LTTNG_LIBS)

GC_V        = $(v_$(V))
GC_CC       = gcc
GC_CFLAGS   = -Wall -flto -fno-strict-aliasing -fvisibility=hidden -Wno-unused $(GC_BUILD_CFLAGS)
GC_CPPFLAGS = -I$(GC_BASE)api $(GC_TRACEPOINT_CPPFLAGS) $(GC_BUILD_CPPFLAGS)
GC_LDFLAGS  = -lpthread -flto=auto $(GC_TRACEPOINT_LIBS)
GC_DEPFLAGS = 
GC_COMPILE  = $(GC_V)$(GC_CC) $(GC_CFLAGS) $(GC_CPPFLAGS) $(GC_DEPFLAGS) -o $@
GC_LINK     = $(GC_V)$(GC_CC) $(GC_LDFLAGS) -o $@
GC_PLATFORM = gnu-linux
GC_OBJDIR   =
GC_EMBEDDER_CPPFLAGS += -DGC_EMBEDDER=\"$(GC_EMBEDDER_H)\"

$(GC_OBJDIR)gc-platform.o: $(GC_BASE)src/gc-platform-$(GC_PLATFORM).c
	$(GC_COMPILE) -c $<
$(GC_OBJDIR)gc-stack.o: $(GC_BASE)src/gc-stack.c
	$(GC_COMPILE) -c $<
$(GC_OBJDIR)gc-options.o: $(GC_BASE)src/gc-options.c
	$(GC_COMPILE) -c $<
$(GC_OBJDIR)gc-tracepoint.o: $(GC_BASE)src/gc-tracepoint.c
	$(GC_COMPILE) -c $<
$(GC_OBJDIR)gc-ephemeron.o: $(GC_BASE)src/gc-ephemeron.c
	$(GC_COMPILE) $(GC_EMBEDDER_CFLAGS) -c $<
$(GC_OBJDIR)gc-finalizer.o: $(GC_BASE)src/gc-finalizer.c
	$(GC_COMPILE) $(GC_EMBEDDER_CFLAGS) -c $<

GC_STEM_bdw   	   = bdw
GC_CPPFLAGS_bdw    = -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1
GC_IMPL_CFLAGS_bdw = `pkg-config --cflags bdw-gc`
GC_LIBS_bdw        = `pkg-config --libs bdw-gc`

GC_STEM_semi       = semi
GC_CPPFLAGS_semi   = -DGC_PRECISE_ROOTS=1
GC_LIBS_semi       = -lm

GC_STEM_pcc        = pcc
GC_CPPFLAGS_pcc    = -DGC_PRECISE_ROOTS=1 -DGC_PARALLEL=1
GC_LIBS_pcc        = -lm

GC_STEM_generational_pcc   = $(GC_STEM_pcc)
GC_CFLAGS_generational_pcc = $(GC_CFLAGS_pcc) -DGC_GENERATIONAL=1
GC_LIBS_generational_pcc   = $(GC_LIBS_pcc)

define mmc_variant
GC_STEM_$(1)       = mmc
GC_CPPFLAGS_$(1)   = $(2)
GC_LIBS_$(1)       = -lm
endef

define generational_mmc_variants
$(call mmc_variant,$(1)mmc,$(2))
$(call mmc_variant,$(1)generational_mmc,$(2) -DGC_GENERATIONAL=1)
endef

define parallel_mmc_variants
$(call generational_mmc_variants,$(1),$(2))
$(call generational_mmc_variants,$(1)parallel_,$(2) -DGC_PARALLEL=1)
endef

define trace_mmc_variants
$(call parallel_mmc_variants,,-DGC_PRECISE_ROOTS=1)
$(call parallel_mmc_variants,stack_conservative_,-DGC_CONSERVATIVE_ROOTS=1)
$(call parallel_mmc_variants,heap_conservative_,-DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1)
endef

$(eval $(call trace_mmc_variants))

gc_var         = $($(1)$(subst -,_,$(2)))
gc_impl        = $(call gc_var,GC_STEM_,$(1)).c
gc_attrs       = $(call gc_var,GC_STEM_,$(1))-attrs.h
gc_cppflags    = $(call gc_var,GC_CPPFLAGS_,$(1))
gc_impl_cflags = $(call gc_var,GC_IMPL_CFLAGS_,$(1))
gc_libs        = $(call gc_var,GC_LIBS_,$(1))

GC_IMPL        	    = $(call gc_impl,$(GC_COLLECTOR))
GC_CPPFLAGS         += $(call gc_cppflags,$(GC_COLLECTOR))
GC_CPPFLAGS         += -DWHIPPET_ATTRS=\"$(GC_BASE)api/$(GC_IMPL)-attrs.h\"
GC_IMPL_CFLAGS 	    = $(call gc_impl_cflags,$(GC_COLLECTOR))
GC_LIBS             = $(call gc_libs,$(GC_COLLECTOR))

$(GC_OBJDIR)gc-impl.o: $(GC_BASE)src/$(call gc_impl,$(GC_COLLECTOR))
	$(GC_COMPILE) $(GC_IMPL_CFLAGS) $(EMBEDDER_TO_GC_CFLAGS) -c $<

GC_OBJS=$(foreach O,gc-platform.o gc-stack.o gc-options.o gc-tracepoint.o gc-ephemeron.o gc-finalizer.o gc-impl.o,$(GC_OBJDIR)$(O))
