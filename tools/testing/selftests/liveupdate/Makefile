# SPDX-License-Identifier: GPL-2.0-only

# --- Test Configuration (Edit this section when adding new tests) ---
LUO_SHARED_SRCS := luo_test_utils.c
LUO_SHARED_HDRS += luo_test_utils.h

LUO_MANUAL_TESTS += luo_multi_file
LUO_MANUAL_TESTS += luo_multi_kexec
LUO_MANUAL_TESTS += luo_multi_session
LUO_MANUAL_TESTS += luo_unreclaimed

LUO_MAIN_TESTS += liveupdate

# --- Automatic Rule Generation (Do not edit below) ---

TEST_GEN_PROGS_EXTENDED += $(LUO_MANUAL_TESTS)
TEST_GEN_PROGS := $(LUO_MAIN_TESTS)

liveupdate_SOURCES := liveupdate.c $(LUO_SHARED_SRCS)

include ../lib.mk

CFLAGS += -Wall -O2 -Wno-unused-function
CFLAGS += $(KHDR_INCLUDES)
LDFLAGS += -static

include lib/libliveupdate.mk

$(OUTPUT)/liveupdate: $(liveupdate_SOURCES) $(LUO_SHARED_HDRS) $(LIBLIVEUPDATE_O)
	$(call msg,LINK,,$@)
	$(Q)$(LINK.c) $^ $(LDLIBS) -o $@

# Define the full list of sources for each manual test.
$(foreach test,$(LUO_MANUAL_TESTS), \
	$(eval $(test)_SOURCES := $(test).c $(LUO_SHARED_SRCS)))

define BUILD_RULE_TEMPLATE
$(OUTPUT)/$(1): $($(1)_SOURCES) $(LUO_SHARED_HDRS) $(LIBLIVEUPDATE_O)
	$(call msg,LINK,,$$@)
	$(Q)$(LINK.c) $$^ $(LDLIBS) -o $$@
	$(Q)chmod +x $$@
endef
# This loop automatically generates an explicit build rule for each manual test.
# It includes dependencies on the shared headers and makes the output
# executable.
# Note the use of '$$' to escape automatic variables for the 'eval' command.
$(foreach test,$(LUO_MANUAL_TESTS), \
	$(eval $(call BUILD_RULE_TEMPLATE,$(test))) \
)
