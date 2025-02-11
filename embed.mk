GC_COLLECTOR ?= semi

DEFAULT_BUILD := opt

BUILD_CFLAGS_opt      = -O2 -g -DNDEBUG
BUILD_CFLAGS_optdebug = -O2 -g -DGC_DEBUG=1
BUILD_CFLAGS_debug    = -O0 -g -DGC_DEBUG=1

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
GC_CPPFLAGS = -I$(WHIPPET)api $(GC_TRACEPOINT_CPPFLAGS)
GC_LDFLAGS  = -lpthread -flto=auto $(GC_TRACEPOINT_LIBS)
GC_DEPFLAGS = 
GC_COMPILE  = $(GC_V)$(GC_CC) $(GC_CFLAGS) $(GC_CPPFLAGS) $(GC_DEPFLAGS) -o $@
GC_LINK     = $(GC_V)$(GC_CC) $(GC_LDFLAGS) -o $@
GC_PLATFORM = gnu-linux
GC_OBJDIR   =

$(GC_OBJDIR)gc-platform.o: $(WHIPPET)src/gc-platform-$(GC_PLATFORM).c
	$(GC_COMPILE) -c $<
$(GC_OBJDIR)gc-stack.o: $(WHIPPET)src/gc-stack.c
	$(GC_COMPILE) -c $<
$(GC_OBJDIR)gc-options.o: $(WHIPPET)src/gc-options.c
	$(GC_COMPILE) -c $<
$(GC_OBJDIR)gc-tracepoint.o: $(WHIPPET)src/gc-tracepoint.c
	$(GC_COMPILE) -c $<
$(GC_OBJDIR)gc-ephemeron.o: $(WHIPPET)src/gc-ephemeron.c
	$(GC_COMPILE) $(EMBEDDER_TO_GC_CFLAGS) -c $<
$(GC_OBJDIR)gc-finalizer.o: $(WHIPPET)src/gc-finalizer.c
	$(GC_COMPILE) $(EMBEDDER_TO_GC_CFLAGS) -c $<

GC_STEM_bdw   	   = bdw
GC_CFLAGS_bdw 	   = -DGC_CONSERVATIVE_ROOTS=1 -DGC_CONSERVATIVE_TRACE=1
GC_IMPL_CFLAGS_bdw = `pkg-config --cflags bdw-gc`
GC_LIBS_bdw        = `pkg-config --libs bdw-gc`

GC_STEM_semi       = semi
GC_CFLAGS_semi     = -DGC_PRECISE_ROOTS=1
GC_LIBS_semi       = -lm

GC_STEM_pcc        = pcc
GC_CFLAGS_pcc      = -DGC_PRECISE_ROOTS=1 -DGC_PARALLEL=1
GC_LIBS_pcc       = -lm

GC_STEM_generational_pcc   = $(GC_STEM_pcc)
GC_CFLAGS_generational_pcc = $(GC_CFLAGS_pcc) -DGC_GENERATIONAL=1
GC_LIBS_generational_pcc   = $(GC_LIBS_pcc)

define mmc_variant
GC_STEM_$(1)       = mmc
GC_CFLAGS_$(1)     = $(2)
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
gc_cflags      = $(call gc_var,GC_CFLAGS_,$(1))
gc_impl_cflags = $(call gc_var,GC_IMPL_CFLAGS_,$(1))
gc_libs        = $(call gc_var,GC_LIBS_,$(1))

GC_IMPL        	    = $(call gc_impl,$(GC_COLLECTOR))
GC_CFLAGS      	   += $(call gc_cflags,$(GC_COLLECTOR))
GC_IMPL_CFLAGS 	    = $(call gc_impl_cflags,$(GC_COLLECTOR))
GC_ATTRS            = $(WHIPPET)api/$(call gc_attrs,$(GC_COLLECTOR))
GC_TO_EMBEDDER_CFLAGS = -include $(GC_ATTRS)
GC_LIBS             = $(call gc_libs,$(GC_COLLECTOR))

$(GC_OBJDIR)gc-impl.o: $(WHIPPET)src/$(call gc_impl,$(GC_COLLECTOR))
	$(GC_COMPILE) $(GC_IMPL_CFLAGS) $(EMBEDDER_TO_GC_CFLAGS) -c $<

GC_OBJS=$(foreach O,gc-platform.o gc-stack.o gc-options.o gc-tracepoint.o gc-ephemeron.o gc-finalizer.o gc-impl.o,$(GC_OBJDIR)$(O))
