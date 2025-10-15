CFLAGS = $(KHDR_INCLUDES)
TEST_GEN_PROGS += vfio_dma_mapping_test
TEST_GEN_PROGS += vfio_iommufd_setup_test
TEST_GEN_PROGS += vfio_pci_device_test
TEST_GEN_PROGS += vfio_pci_driver_test
TEST_GEN_PROGS_EXTENDED += vfio_pci_liveupdate_test
TEST_PROGS_EXTENDED := run.sh
include ../lib.mk
include lib/libvfio.mk
include ../liveupdate/lib/libliveupdate.mk

CFLAGS += -I$(top_srcdir)/tools/include
CFLAGS += -MD
CFLAGS += $(EXTRA_CFLAGS)

LIBS_O := $(LIBVFIO_O)
LIBS_O += $(LIBLIVEUPDATE_O)

TEST_GEN_ALL_PROGS := $(TEST_GEN_PROGS)
TEST_GEN_ALL_PROGS += $(TEST_GEN_PROGS_EXTENDED)

$(TEST_GEN_ALL_PROGS): %: %.o $(LIBS_O)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(TARGET_ARCH) $< $(LIBS_O) $(LDLIBS) -o $@

TEST_GEN_PROGS_O = $(patsubst %, %.o, $(TEST_GEN_PROGS))
TEST_GEN_PROGS_O += $(patsubst %, %.o, $(TEST_GEN_PROGS_EXTENDED))
TEST_DEP_FILES = $(patsubst %.o, %.d, $(TEST_GEN_PROGS_O) $(LIBS_O))
-include $(TEST_DEP_FILES)

EXTRA_CLEAN += $(TEST_GEN_PROGS_O) $(TEST_DEP_FILES)
