# Define the source files and output file.
SOURCE_FILES := src/graphgrad.cc
HEADER_FILES := src/Tensor.h
INPUT_FILES := $(SOURCE_FILES) $(HEADER_FILES)
OUTPUT_FILE := graphgrad$(shell python3-config --extension-suffix)

# The default target builds the output file.
all: $(OUTPUT_FILE)

# Build the output file.
$(OUTPUT_FILE): $(INPUT_FILES)
	g++ \
		$(SOURCE_FILES) \
		-o graphgrad$$(python3-config --extension-suffix) \
		-Wall -std=c++17 \
		-fPIC -shared $$(python3 -m pybind11 --includes) \
		-O3

# The clean target removes the output file.
clean:
	rm -f $(OUTPUT_FILE)

.PHONY: clean all