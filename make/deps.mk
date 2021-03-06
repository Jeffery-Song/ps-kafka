# Install dependencies

URL=https://raw.githubusercontent.com/mli/deps/master/build
GBURL=https://raw.githubusercontent.com/gbxu/deps/master/build
ifndef WGET
WGET = wget
endif

# protobuf
PROTOBUF = ${DEPS_PATH}/include/google/protobuf/message.h
${PROTOBUF}:
	$(eval FILE=protobuf-2.5.0.tar.gz)
	$(eval DIR=protobuf-2.5.0)
	rm -rf $(FILE) $(DIR)
	$(WGET) $(URL)/$(FILE) && tar --no-same-owner -zxf $(FILE)
	cd $(DIR) && export CFLAGS=-fPIC && export CXXFLAGS=-fPIC && ./configure -prefix=$(DEPS_PATH) && $(MAKE) && $(MAKE) install
	rm -rf $(FILE) $(DIR)

#[----
# #cyrus-sasl-2.1.27
# CYRUSSASL = ${DEPS_PATH}/include/
# ${CYRUSSASL}:
#	$(eval FILE=cyrus-sasl-2.1.27.tar.gz)
#	$(eval DIR=cyrus-sasl-2.1.27)
#	rm -rf $(FILE) $(DIR)
#	$(WGET) $(GBURL)/$(FILE) && tar --no-same-owner -zxf $(FILE)
#	cd $(DIR) && ./autogen.sh  -prefix=$(DEPS_PATH) && $(MAKE) && $(MAKE) install
#	rm -rf $(FILE) $(DIR)

# #openssl-1.1.0h
# OPENSSL = ${DEPS_PATH}/include/
# ${OPENSSL}:
#	$(eval FILE=openssl-1.1.0h.tar.gz)
#	$(eval DIR=openssl-1.1.0h)
#	rm -rf $(FILE) $(DIR)
#	$(WGET) $(GBURL)/$(FILE) && tar --no-same-owner -zxf $(FILE)
#	cd $(DIR) && ./Configure -prefix=$(DEPS_PATH) && $(MAKE) && $(MAKE) install
#	rm -rf $(FILE) $(DIR)

# #zlib-1.2.11
# ZLIB = ${DEPS_PATH}/include/
# ${ZLIB}:
#	$(eval FILE=zlib-1.2.11.tar.gz)
#	$(eval DIR=zlib-1.2.11)
#	rm -rf $(FILE) $(DIR)
#	$(WGET) $(GBURL)/$(FILE) && tar --no-same-owner -zxf $(FILE)
#	cd $(DIR) && ./Configure -prefix=$(DEPS_PATH) && $(MAKE) && $(MAKE) install
#	rm -rf $(FILE) $(DIR)

# rdkafka
RDKAFKA = ${DEPS_PATH}/include/librdkafka/rdkafka.h
${RDKAFKA}:
	$(eval FILE=rdkafka.tar.gz)
	$(eval DIR=rdkafka)
	rm -rf $(FILE) $(DIR)
	$(WGET) $(GBURL)/$(FILE) && tar --no-same-owner -zxf $(FILE)
	cd $(DIR) && ./configure --prefix=$(DEPS_PATH)  --disable-lz4 --disable-ssl --disable-sasl && $(MAKE) && $(MAKE) install
	rm -rf $(FILE) $(DIR)
#----]

# lz4
LZ4 = ${DEPS_PATH}/include/lz4.h
${LZ4}:
	$(eval FILE=lz4-r129.tar.gz)
	$(eval DIR=lz4-r129)
	rm -rf $(FILE) $(DIR)
	wget $(URL)/$(FILE) && tar --no-same-owner -zxf $(FILE)
	cd $(DIR) && $(MAKE) && PREFIX=$(DEPS_PATH) $(MAKE) install
	rm -rf $(FILE) $(DIR)

# cityhash
CITYHASH = ${DEPS_PATH}/include/city.h
${CITYHASH}:
	$(eval FILE=cityhash-1.1.1.tar.gz)
	$(eval DIR=cityhash-1.1.1)
	rm -rf $(FILE) $(DIR)
	wget $(URL)/$(FILE) && tar --no-same-owner -zxf $(FILE)
	cd $(DIR) && ./configure -prefix=$(DEPS_PATH) --enable-sse4.2 && $(MAKE) CXXFLAGS="-g -O3 -msse4.2" && $(MAKE) install
	rm -rf $(FILE) $(DIR)


# # gflags
# ${DEPS_PATH}/include/google/gflags.h:
# 	$(eval FILE=gflags-2.0-no-svn-files.tar.gz)
# 	$(eval DIR=gflags-2.0)
# 	rm -rf $(FILE) $(DIR)
# 	wget $(URL)/$(FILE) && tar -zxf $(FILE)
# 	cd $(DIR) && ./configure -prefix=$(DEPS_PATH) && $(MAKE) && $(MAKE) install
# 	rm -rf $(FILE) $(DIR)
# gflags: | ${DEPS_PATH}/include/google/gflags.h

# # glog
# ${DEPS_PATH}/include/glog/logging.h: | ${DEPS_PATH}/include/google/gflags.h
# 	$(eval FILE=v0.3.4.tar.gz)
# 	$(eval DIR=glog-0.3.4)
# 	rm -rf $(FILE) $(DIR)
# 	wget https://github.com/google/glog/archive/$(FILE) && tar -zxf $(FILE)
# 	cd $(DIR) && ./configure -prefix=$(DEPS_PATH) --with-gflags=$(DEPS_PATH) && $(MAKE) && $(MAKE) install
# 	rm -rf $(FILE) $(DIR)
# glog: | ${DEPS_PATH}/include/glog/logging.h
