# 电力现货市场出清仿真平台 —— 构建脚本
CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra
TARGET   := spot_platform
SRCS     := main.cpp market.cpp
HDRS     := market.h
CSVS     := bids_gen.csv bids_con.csv clearing_96.csv settlement.csv quadratic.csv

.PHONY: all run test clean

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

run: $(TARGET)
	./$(TARGET)

test: $(TARGET)
	./$(TARGET) --selftest

clean:
	rm -f $(TARGET) $(CSVS)
