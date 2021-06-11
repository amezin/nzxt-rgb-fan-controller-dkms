#!/usr/bin/env python3


BYTE_ORDER = 'little'


def number_length_bytes(min_value, max_value):
	signed = min_value < 0

	for length in range(1, 8):
		try:
			min_value.to_bytes(length, byteorder=BYTE_ORDER, signed=signed)
			max_value.to_bytes(length, byteorder=BYTE_ORDER, signed=signed)
			return length

		except OverflowError:
			pass


def do_find(min_value, max_value, input):
	if (max_value < min_value):
		raise ValueError('min should be less than max')

	input_bytes = input.read()
	length = number_length_bytes(min_value, max_value)
	print(f'Number length {length} bytes')

	for value in range(min_value, max_value + 1):
		value_bytes = value.to_bytes(length, byteorder=BYTE_ORDER, signed=min_value < 0)

		offset = -1
		while True:
			offset = input_bytes.find(value_bytes, offset + 1)
			if offset == -1:
				break

			print(f'Found {value} at offset {offset}')


def main():
	import argparse
	parser = argparse.ArgumentParser()
	parser.add_argument('--min', dest='min_value', type=int, required=True)
	parser.add_argument('--max', dest='max_value', type=int, required=True)
	parser.add_argument('input', type=argparse.FileType('rb'))
	do_find(**vars(parser.parse_args()))


if __name__ == '__main__':
	main()
