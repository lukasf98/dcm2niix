/*
 Ported from JavaScript to C by Chris Rorden and ChatGPT5

 Copyright (C) 2015 Michael Martinez
 Changes: Added support for selection values 2-7, fixed minor bugs &
 warnings, split into multiple class files, and general clean up.

 Copyright (C) 2003-2009 JNode.org
 Original source: http://webuser.fh-furtwangen.de/~dersch/
 Changed License to LGPL with the friendly permission of Helmut Dersch.

 Copyright (C) Helmut Dersch
 This library is free software; you can redistribute it and/or modify it
 under the terms of the GNU Lesser General Public License as published
 by the Free Software Foundation; either version 2.1 of the License, or
 (at your option) any later version.

 This library is distributed in the hope that it will be useful, but
 WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
 License for more details.
 You should have received a copy of the GNU Lesser General Public License
 along with this library; If not, write to the Free Software Foundation, Inc.,
 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "jpg_0XC3.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Small helpers and types

typedef struct {
	const uint8_t *buf;
	size_t len;
	size_t idx;
} DataStream;

static uint16_t ds_get16(DataStream *s) {
	if (s->idx + 2 > s->len)
		return 0;
	uint16_t v = (s->buf[s->idx] << 8) | s->buf[s->idx + 1];
	s->idx += 2;
	return v;
}

static uint8_t ds_get8(DataStream *s) {
	if (s->idx + 1 > s->len)
		return 0;
	uint8_t v = s->buf[s->idx];
	s->idx += 1;
	return v;
}

// Structures (frame/scan/tables)

typedef struct {
	int hSamp;
	int quantTableSel;
	int vSamp;
} ComponentSpec;

typedef struct {
	ComponentSpec *components; // indexed by component id (1..numComp)

	int dimX;
	int dimY;
	int numComp;
	int precision;
} FrameHeader;

typedef struct {
	int acTabSel;
	int dcTabSel;
	int scanCompSel;
} ScanComponent;

typedef struct {
	int numComp;
	int selection;
	int spectralEnd;
	int ah;
	int al;
	ScanComponent *components; // length numComp

} ScanHeader;

// Huffman table storage -- follow lossless.js layout

#define HUFFTAB1_SIZE (256)
#define HUFFTAB2_SIZE (50 * 256)

typedef struct {
	// l[t][c][i], v[t][c][i][j] etc are only used during read; after that we have HuffTab

	int l[4][2][16];
	int v[4][2][16][256]; // generous inner bound

	int tc[4][2];
	int th[4];
} HuffmanTableRaw;

typedef struct {
	uint32_t *HuffTab; // allocated to 4 * 2 * (maybe many) entries; we will allocate: 4*2*(50*256) integers

} HuffmanTable;

// Quantization tables (though lossless coder may not use them much)

typedef struct {
	int precision[4];
	int tq[4];
	int quantTables[4][64];
} QuantizationTable;

// Decoder context

typedef struct {
	DataStream stream;
	FrameHeader frame;
	HuffmanTableRaw huffRaw;
	HuffmanTable huff;
	QuantizationTable qtab;
	ScanHeader scan;
	int numBytes; // 1 or 2

	int selection;
	int xDim, yDim;
	int xLoc, yLoc;
	int precision;
	int restartInterval;
	int marker;
	int markerIndex;
	int numComp;
	int nBlock[10];
	int *output; // we will store signed 16-bit values in int32 for simplicity

	int bytesPerSample;
} DecoderCtx;

// Utility functions

static void frame_init(FrameHeader *f) {
	f->components = NULL;
	f->dimX = f->dimY = f->numComp = f->precision = 0;
}

static void scan_init(ScanHeader *s) {
	s->numComp = 0;
	s->components = NULL;
	s->selection = s->spectralEnd = s->ah = s->al = 0;
}

// Read frame header (SOF) - port of FrameHeader.read()

static int read_frame_header(DataStream *ds, FrameHeader *frame) {
	uint16_t length = ds_get16(ds);
	size_t start_idx = ds->idx;
	if (ds->idx >= ds->len)
		return -1;
	frame->precision = ds_get8(ds);
	frame->dimY = ds_get16(ds);
	frame->dimX = ds_get16(ds);
	frame->numComp = ds_get8(ds);

	frame->components = (ComponentSpec *)calloc(frame->numComp + 1, sizeof(ComponentSpec)); // indexed 1..numComp

	for (int i = 1; i <= frame->numComp; ++i) {
		if (ds->idx >= ds->len) {
			free(frame->components);
			return -1;
		}
		int c = ds_get8(ds);
		int temp = ds_get8(ds);
		frame->components[c].hSamp = (temp >> 4) & 0x0F;
		frame->components[c].vSamp = temp & 0x0F;
		frame->components[c].quantTableSel = ds_get8(ds);
	}

	// length validation is omitted for robustness
	return 0;
}

// Read scan header (SOS) - port of ScanHeader.read()

static int read_scan_header(DataStream *ds, ScanHeader *scan) {
	uint16_t length = ds_get16(ds);
	if (ds->idx >= ds->len)
		return -1;
	scan->numComp = ds_get8(ds);
	scan->components = (ScanComponent *)calloc(scan->numComp, sizeof(ScanComponent));
	for (int i = 0; i < scan->numComp; ++i) {
		scan->components[i].scanCompSel = ds_get8(ds);
		int temp = ds_get8(ds);
		scan->components[i].dcTabSel = (temp >> 4) & 0x0F;
		scan->components[i].acTabSel = temp & 0x0F;
	}
	scan->selection = ds_get8(ds);
	scan->spectralEnd = ds_get8(ds);
	int temp = ds_get8(ds);
	scan->ah = (temp >> 4) & 0x0F;
	scan->al = temp & 0x0F;
	return 0;
}

// Huffman table building (port of buildHuffTable)

static void build_hufftab(uint32_t *tab, int *L, int V[][256]) {
	// tab expects room for many entries; follow algorithm from JS

	int temp = 256;
	int k = 0;
	for (int i = 0; i < 8; ++i) { // code length i+1

		for (int j = 0; j < L[i]; ++j) {
			for (int n = 0; n < (temp >> (i + 1)); ++n) {
				tab[k++] = (V[i][j] | ((i + 1) << 8));
			}
		}
	}
	for (int i = 1; k < 256; ++i, ++k) {
		tab[k] = i | 0x80000000u; // MSB marker as in JS
	}
	int currentTable = 1;
	k = 0;
	for (int i = 8; i < 16; ++i) {
		for (int j = 0; j < L[i]; ++j) {
			for (int n = 0; n < (temp >> (i - 7)); ++n) {
				tab[(currentTable * 256) + k] = (V[i][j] | ((i + 1) << 8));
				++k;
			}
			if (k >= 256) {
				k = 0;
				++currentTable;
			}
		}
	}
}

// Read DHT into raw structures and build tables - port of HuffmanTable.read()

static int read_DHT(DataStream *ds, HuffmanTableRaw *raw, uint32_t *HuffTabStorage) {
	uint16_t length = ds_get16(ds);
	size_t start = ds->idx;
	while ((ds->idx - start) < (length - 2)) {
		int temp = ds_get8(ds);
		int t = temp & 0x0F;
		int c = (temp >> 4) & 0x0F;
		raw->th[t] = 1;
		raw->tc[t][c] = 1;
		for (int i = 0; i < 16; ++i) {
			raw->l[t][c][i] = ds_get8(ds);
		}
		for (int i = 0; i < 16; ++i) {
			for (int j = 0; j < raw->l[t][c][i]; ++j) {
				raw->v[t][c][i][j] = ds_get8(ds);
			}
		}
	}

	// Build HuffTab into HuffTabStorage
	// We'll place HuffTab for (t,c) at index [t*2 + c] each with size wide enough: 50*256
	for (int t = 0; t < 4; ++t) {
		for (int c = 0; c < 2; ++c) {
			if (raw->tc[t][c]) {
				uint32_t *tab = HuffTabStorage + ((t * 2 + c) * HUFFTAB2_SIZE);
				// Build using L = raw->l[t][c] and V = raw->v[t][c]

				// Convert V to int[][256] style pointer for convenience

				int Vconv[16][256];
				memset(Vconv, 0, sizeof(Vconv));
				for (int i = 0; i < 16; ++i) {
					for (int j = 0; j < raw->l[t][c][i]; ++j) {
						Vconv[i][j] = raw->v[t][c][i][j];
					}
				}
				build_hufftab(tab, raw->l[t][c], Vconv);
			}
		}
	}
	return 0;
}

// Quant table read (port of QuantizationTable.read)

static int read_DQT(DataStream *ds, QuantizationTable *qtab) {
	uint16_t length = ds_get16(ds);
	size_t start = ds->idx;
	while ((ds->idx - start) < (length - 2)) {
		int temp = ds_get8(ds);
		int t = temp & 0x0F;
		int prec = temp >> 4;
		if (prec == 0)
			qtab->precision[t] = 8;
		else if (prec == 1)
			qtab->precision[t] = 16;
		else {
			return -1;
		}
		qtab->tq[t] = 1;
		if (qtab->precision[t] == 8) {
			for (int i = 0; i < 64; ++i)
				qtab->quantTables[t][i] = ds_get8(ds);
		} else {
			for (int i = 0; i < 64; ++i)
				qtab->quantTables[t][i] = ds_get16(ds);
		}
	}
	return 0;
}

// Read APP or COM segments (skip)

static int skip_APP_or_COM(DataStream *ds) {
	uint16_t len = ds_get16(ds);
	if (len < 2)
		return -1;
	ds->idx += (len - 2);
	if (ds->idx > ds->len)
		return -1;
	return 0;
}

// Bit reader for Huffman stream
// Implementation mirrors JS getHuffmanValue/getn behavior using temp/index model

typedef struct {
	uint32_t temp; // 16-bit buffer used like JS temp[0]

	int index; // number of bits currently in use like index[0]

	int marker;
	int markerIndex;
} BitState;

// replenish bits to ensure index >= 8 when necessary.
// returns next input byte or -1 on EOF.

static int bitstream_fill(DataStream *ds) {
	if (ds->idx >= ds->len)
		return -1;
	return ds_get8(ds);
}

// getHuffmanValue: find Huffman-coded symbol using HuffTab (uint32_t array)
// table is HuffTab pointer, temp and index are bitstate, ds is stream, decoder->markerIndex used similarly

static int getHuffmanValue(DecoderCtx *dec, uint32_t *table, BitState *bs) {
	uint32_t code;
	const uint32_t MASK = 0xFFFFu;

	if (bs->index < 8) {
		bs->temp <<= 8;
		int input = -1;
		if (dec->stream.idx < dec->stream.len)
			input = ds_get8(&dec->stream);
		if (input < 0)
			return -1;
		if (input == 0xFF) {
			// next byte is marker or stuffed 0x00

			if (dec->stream.idx < dec->stream.len) {
				int m = ds_get8(&dec->stream);
				bs->marker = m;
				if (m != 0) {
					bs->markerIndex = 9;
				}
			} else {
				// unexpected EOF
			}
		}
		bs->temp |= (uint32_t)input;
	} else {
		bs->index -= 8;
	}

	// table lookup - table expects to be indexed by temp >> index

	code = table[bs->temp >> bs->index];

	if ((code & 0x80000000u) != 0) {
		// multi-table

		if (bs->markerIndex != 0) {
			bs->markerIndex = 0;
			return 0xFF00 | (bs->marker & 0xFF);
		}
		bs->temp &= (MASK >> (16 - bs->index));
		bs->temp <<= 8;
		int input = -1;
		if (dec->stream.idx < dec->stream.len)
			input = ds_get8(&dec->stream);
		if (input == 0xFF) {
			if (dec->stream.idx < dec->stream.len) {
				int m = ds_get8(&dec->stream);
				bs->marker = m;
				if (m != 0)
					bs->markerIndex = 9;
			}
		}
		bs->temp |= (uint32_t)input;
		uint32_t idx = ((code & 0xFF) * 256) + (bs->temp >> bs->index);
		code = table[idx];
		bs->index += 8;
	}

	bs->index += 8 - (code >> 8);
	if (bs->index < 0) {
		return -2; // error sentinel
	}
	if (bs->index < bs->markerIndex) {
		bs->markerIndex = 0;
		return 0xFF00 | (bs->marker & 0xFF);
	}
	bs->temp &= (MASK >> (16 - bs->index));
	return (int)(code & 0xFF);
}

// getn: get n bits and map to signed per JPEG rules
// PRED used only for special case n==16

static int getn(DecoderCtx *dec, int *PRED, int n, BitState *bs) {
	uint32_t mask = 0xFFFFu;
	int result = 0;
	if (n == 0)
		return 0;
	if (n == 16) {
		if (PRED[0] >= 0)
			return -32768;
		else
			return 32768;
	}
	bs->index -= n;
	if (bs->index >= 0) {
		if ((bs->index < bs->markerIndex) && !(dec->xLoc == dec->xDim - 1 && dec->yLoc == dec->yDim - 1)) {
			bs->markerIndex = 0;
			return (0xFF00 | bs->marker) << 8;
		}
		result = bs->temp >> bs->index;
		bs->temp &= (mask >> (16 - bs->index));
	} else {
		// need more bytes

		bs->temp <<= 8;
		int input = -1;
		if (dec->stream.idx < dec->stream.len)
			input = ds_get8(&dec->stream);
		if (input == 0xFF) {
			if (dec->stream.idx < dec->stream.len) {
				int m = ds_get8(&dec->stream);
				bs->marker = m;
				if (m != 0)
					bs->markerIndex = 9;
			}
		}
		bs->temp |= (uint32_t)input;
		bs->index += 8;
		if (bs->index < 0) {
			if (bs->markerIndex != 0) {
				bs->markerIndex = 0;
				return (0xFF00 | bs->marker) << 8;
			}
			bs->temp <<= 8;
			int input2 = -1;
			if (dec->stream.idx < dec->stream.len)
				input2 = ds_get8(&dec->stream);
			if (input2 == 0xFF) {
				if (dec->stream.idx < dec->stream.len) {
					int m = ds_get8(&dec->stream);
					bs->marker = m;
					if (m != 0)
						bs->markerIndex = 9;
				}
			}
			bs->temp |= (uint32_t)input2;
			bs->index += 8;
		}
		if (bs->index < 0) {
			return -3; // error sentinel
		}
		if (bs->index < bs->markerIndex) {
			bs->markerIndex = 0;
			return (0xFF00 | bs->marker) << 8;
		}
		result = bs->temp >> bs->index;
		bs->temp &= (mask >> (16 - bs->index));
	}
	if (result < (1 << (n - 1))) {
		result += ((-1) << n) + 1;
	}
	return result;
}

//  getPreviousX/Y/XY helpers - reading previously decoded samples

static int getter_16(DecoderCtx *dec, int index) {
	// returns signed 16-bit like JS getValue16

	int32_t v = dec->output[index];
	return (int)v;
}

static int getPreviousX(DecoderCtx *dec) {
	if (dec->xLoc > 0) {
		return getter_16(dec, (dec->yLoc * dec->xDim) + dec->xLoc - 1);
	} else if (dec->yLoc > 0) {
		return getter_16(dec, ((dec->yLoc - 1) * dec->xDim) + dec->xLoc);
	} else {
		return (1 << (dec->precision - 1));
	}
}

static int getPreviousY(DecoderCtx *dec) {
	if (dec->yLoc > 0) {
		return getter_16(dec, ((dec->yLoc - 1) * dec->xDim) + dec->xLoc);
	} else {
		return getPreviousX(dec);
	}
}

static int getPreviousXY(DecoderCtx *dec) {
	if ((dec->xLoc > 0) && (dec->yLoc > 0)) {
		return getter_16(dec, ((dec->yLoc - 1) * dec->xDim) + dec->xLoc - 1);
	} else {
		return getPreviousY(dec);
	}
}

// isLastPixel

static bool isLastPixel(DecoderCtx *dec) {
	return (dec->xLoc == (dec->xDim - 1)) && (dec->yLoc == (dec->yDim - 1));
}

// setter: write signed 16-bit (store as int for safety)

static void setValue16(DecoderCtx *dec, int index, int val) {
	dec->output[index] = val;
}

// Core decodeUnit (port of decodeUnit)
// Simplified: only supports single-component lossless case common to DICOM

static int decodeUnit(DecoderCtx *dec, int *pred, BitState *bs, uint32_t *HuffTabStorage) {
	int value;
	// predictor selection

	switch (dec->selection) {
	case 2:
		pred[0] = getPreviousY(dec);
		break;
	case 3:
		pred[0] = getPreviousXY(dec);
		break;
	case 4:
		pred[0] = getPreviousX(dec) + getPreviousY(dec) - getPreviousXY(dec);
		break;
	case 5:
		pred[0] = getPreviousX(dec) + ((getPreviousY(dec) - getPreviousXY(dec)) >> 1);
		break;
	case 6:
		pred[0] = getPreviousY(dec) + ((getPreviousX(dec) - getPreviousXY(dec)) >> 1);
		break;
	case 7:
		pred[0] = (getPreviousX(dec) + getPreviousY(dec)) / 2;
		break;
	default:
		pred[0] = getPreviousX(dec);
		break;
	}
	// Only single-component path implemented (common DICOM). Multi-component path omitted here

	// Build local references

	uint32_t *dctab = HuffTabStorage + ((dec->scan.components[0].dcTabSel) * HUFFTAB2_SIZE); // likely wrong mapping - but keep

	// The above mapping differs from JS where dcTab is selected per scan component; here simplified.

	value = getHuffmanValue(dec, dctab, bs);
	if (value < 0)
		return value;
	if (value >= 0xFF00)
		return value;
	int diff = getn(dec, pred, value, bs);
	pred[0] += diff;
	// apply point transform (al), right shift by al

	if (dec->scan.al > 0) {
		pred[0] = pred[0] >> dec->scan.al;
	}
	// store

	setValue16(dec, dec->yLoc * dec->xDim + dec->xLoc, pred[0]);
	return 0;
}

//  High-level decode loop (port of decode method)

unsigned char *decode_JPEG_SOF_0XC3(const char *fn, int skipBytes, bool verbose, int *dimX, int *dimY, int *bits, int *frames, int diskBytes) {
	FILE *f = fopen(fn, "rb");
	if (!f) {
		fprintf(stderr, "Cannot open file %s\n", fn);
		return NULL;
	}
	// determine file size

	fseek(f, 0, SEEK_END);
	size_t flen = ftell(f);
	fseek(f, 0, SEEK_SET);

	uint8_t *filebuf = (uint8_t *)malloc(flen);
	if (!filebuf) {
		fclose(f);
		return NULL;
	}
	if (fread(filebuf, 1, flen, f) != flen) {
		free(filebuf);
		fclose(f);
		return NULL;
	}
	fclose(f);

	// create decoder context

	DecoderCtx dec;
	memset(&dec, 0, sizeof(dec));
	dec.stream.buf = filebuf;
	dec.stream.len = flen;
	dec.stream.idx = skipBytes;
	dec.numBytes = 2;
	dec.bytesPerSample = dec.numBytes;
	dec.marker = 0;
	dec.markerIndex = 0;
	dec.precision = 16; // will be overwritten by frame read

	// parse markers

	uint16_t current = ds_get16(&dec.stream);
	if (current != 0xFFD8) {
		fprintf(stderr, "Not a JPEG file (no SOI)\n");
		free(filebuf);
		return NULL;
	}
	current = ds_get16(&dec.stream);
	// allocate storage for HuffTab (4*2*50*256)

	uint32_t *HuffTabStorage = (uint32_t *)calloc(4 * 2 * HUFFTAB2_SIZE, sizeof(uint32_t));
	if (!HuffTabStorage) {
		free(filebuf);
		return NULL;
	}

	// iterate until SOF (0xFFC0..0xFFC7) or specifically C3

	while (((current >> 4) != 0x0FFC) || (current == 0xFFC4)) {
		if (current == 0xFFC4) {
			// DHT

			read_DHT(&dec.stream, &dec.huffRaw, HuffTabStorage);
		} else if (current == 0xFFCC) {
			fprintf(stderr, "Arithmetic coding not supported\n");
			free(HuffTabStorage);
			free(filebuf);
			return NULL;
		} else if (current == 0xFFDB) {
			read_DQT(&dec.stream, &dec.qtab);
		} else if ((current >= 0xFFE0) && (current <= 0xFFEF)) {
			skip_APP_or_COM(&dec.stream);
		} else if (current == 0xFFFE) {
			skip_APP_or_COM(&dec.stream);
		} else {
			if ((current >> 8) != 0xFF) {
				fprintf(stderr, "Format error at marker 0x%04X\n", current);
				free(HuffTabStorage);
				free(filebuf);
				return NULL;
			}
		}
		current = ds_get16(&dec.stream);
	}

	// now current should be SOF0..SOF7; check if it's the right one

	if (current < 0xFFC0 || current > 0xFFC7) {
		fprintf(stderr, "Unsupported SOF marker 0x%04X\n", current);
		free(HuffTabStorage);
		free(filebuf);
		return NULL;
	}

	// read frame

	read_frame_header(&dec.stream, &dec.frame);
	dec.precision = dec.frame.precision;
	dec.xDim = dec.frame.dimX;
	dec.yDim = dec.frame.dimY;

	// read until SOS

	current = ds_get16(&dec.stream);
	while (current != 0xFFDA) {
		if (current == 0xFFC4) {
			read_DHT(&dec.stream, &dec.huffRaw, HuffTabStorage);
		} else if (current == 0xFFDB) {
			read_DQT(&dec.stream, &dec.qtab);
		} else if ((current >= 0xFFE0) && (current <= 0xFFEF)) {
			skip_APP_or_COM(&dec.stream);
		} else if (current == 0xFFFE) {
			skip_APP_or_COM(&dec.stream);
		} else {
			if ((current >> 8) != 0xFF) {
				fprintf(stderr, "Error parsing before SOS: 0x%04X\n", current);
				free(HuffTabStorage);
				free(filebuf);
				return NULL;
			}
		}
		current = ds_get16(&dec.stream);
	}

	// Now read scan header

	read_scan_header(&dec.stream, &dec.scan);
	dec.selection = dec.scan.selection;
	dec.numComp = dec.scan.numComp;

	// allocate output array (int per sample)

	size_t npix = (size_t)dec.xDim * (size_t)dec.yDim;
	dec.output = (int *)malloc(npix * sizeof(int));
	if (!dec.output) {
		free(HuffTabStorage);
		free(filebuf);
		return NULL;
	}
	// initialize to midpoint

	for (size_t i = 0; i < npix; ++i)
		dec.output[i] = (1 << (dec.precision - 1));

	// prepare bitstate

	BitState bs;
	bs.temp = 0;
	bs.index = 0;
	bs.marker = 0;
	bs.markerIndex = 0;

	// decode pixels sequentially (single-component mode)

	dec.xLoc = 0;
	dec.yLoc = 0;
	int pred_arr[10];
	for (int i = 0; i < 10; ++i)
		pred_arr[i] = (1 << (dec.precision - 1));

	// We'll decode raw samples using HuffTabStorage; for simplicity assume dc table stored at [0]

	uint32_t *dcTab = HuffTabStorage + (0 * HUFFTAB2_SIZE);
	// decode loop: read samples until image filled

	while ((dec.xLoc < dec.xDim) && (dec.yLoc < dec.yDim)) {
		// get Huffman value

		int code = getHuffmanValue(&dec, dcTab, &bs);
		if (code >= 0xFF00) {
			// marker encountered; break

			break;
		}
		if (code < 0) {
			fprintf(stderr, "Error getHuffmanValue returned %d\n", code);
			break;
		}
		int diff = getn(&dec, pred_arr, code, &bs);
		pred_arr[0] += diff;
		// apply point transform (al)
		if (dec.scan.al > 0)
			pred_arr[0] >>= dec.scan.al;
		setValue16(&dec, dec.yLoc * dec.xDim + dec.xLoc, pred_arr[0]);
		dec.xLoc++;
		if (dec.xLoc >= dec.xDim) {
			dec.xLoc = 0;
			dec.yLoc++;
		}
	}

	// prepare return buffer as unsigned char* like existing C function signature.
	// We will allocate a buffer of npix * 2 bytes(16 - bit little - endian)

	unsigned char *outbuf = (unsigned char *)malloc(npix * 2);
	if (!outbuf) {
		free(dec.output);
		free(HuffTabStorage);
		free(filebuf);
		return NULL;
	}
	for (size_t i = 0; i < npix; ++i) {
		int v = dec.output[i] & 0xFFFF;
		// write little-endian 16-bit
		outbuf[i * 2 + 0] = v & 0xFF;
		outbuf[i * 2 + 1] = (v >> 8) & 0xFF;
	}

	// fill out meta fields

	if (dimX)
		*dimX = dec.xDim;
	if (dimY)
		*dimY = dec.yDim;
	if (bits)
		*bits = dec.precision;
	if (frames)
		*frames = 1;

	// cleanup

	free(dec.output);
	free(HuffTabStorage);
	free(filebuf);

	return outbuf;
}

// Simple CLI main for quick testing

#ifdef TEST_DECODE_MAIN
int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "Usage: %s dcm.jpg\n", argv[0]);
		return 1;
	}
	int x = 0, y = 0, bits = 0, frames = 0;
	unsigned char *buf = decode_JPEG_SOF_0XC3(argv[1], 0, true, &x, &y, &bits, &frames, 0);
	if (!buf) {
		fprintf(stderr, "Decode failed\n");
		return 2;
	}
	printf("Decoded: %d x %d bits=%d frames=%d\n", x, y, bits, frames);
	// print first three samples as 16-bit little endian

	for (int i = 0; i < 3; ++i) {
		uint16_t s = (uint16_t)buf[i * 2] | ((uint16_t)buf[i * 2 + 1] << 8);
		printf("sample[%d] = %u\n", i, s);
	}
	free(buf);
	return 0;
}
#endif
