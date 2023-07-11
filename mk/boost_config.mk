# CHANGME - set the path to your boost installation
BOOST_INSTALL_DIR=$(HOME)/package-install/boost_1_82_0
BOOST_CXXFLAGS += -I$(BOOST_INSTALL_DIR)/
BOOST_LDFLAGS  += -L$(BOOST_INSTALL_DIR)/lib -Wl,-rpath=$(BOOST_INSTALL_DIR)/lib

