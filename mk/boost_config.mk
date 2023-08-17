# CHANGME - set the path to your boost installation
BOOST_INSTALL_DIR=/install
BOOST_CXXFLAGS += -I$(BOOST_INSTALL_DIR)/include
BOOST_LDFLAGS  += -L$(BOOST_INSTALL_DIR)/lib -Wl,-rpath=$(BOOST_INSTALL_DIR)/lib

