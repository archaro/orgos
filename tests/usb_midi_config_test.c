#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usb_midi.h"

typedef struct {
  uint8_t data[1024];
  unsigned int length;
  unsigned int midi_interface;
  unsigned int header;
  unsigned int jacks;
  unsigned int out_endpoint;
  unsigned int out_association;
  unsigned int in_endpoint;
  unsigned int in_association;
} config_t;

static void append(config_t *config, const uint8_t *data, unsigned int length) {
  assert(config->length + length <= sizeof(config->data));
  memcpy(config->data + config->length, data, length);
  config->length += length;
}

static void set_length(config_t *config) {
  config->data[2] = (uint8_t)config->length;
  config->data[3] = (uint8_t)(config->length >> 8U);
}

static void association(config_t *config, unsigned int cables, bool input) {
  uint8_t descriptor[4U + USB_MIDI_MAX_CABLES + 1U] = {
    (uint8_t)(4U + cables), 0x25U, 0x01U, (uint8_t)cables
  };
  assert(cables <= USB_MIDI_MAX_CABLES + 1U);
  for (unsigned int i = 0U; i < cables; i++)
    descriptor[4U + i] = (uint8_t)(4U * i + (input ? 3U : 1U));
  append(config, descriptor, 4U + cables);
}

// Audio Control + MIDIStreaming, with embedded/external jack pairs and
// the existing endpoint addresses (OUT 02 / IN 81), alt 0 and 64-byte MPS.
// Nine-byte audio endpoint descriptors preserve the CME U2MIDI Pro format;
// seven-byte standard endpoint descriptors are also already accepted.
static config_t configuration(unsigned int in_cables, unsigned int out_cables,
    unsigned int endpoint_size) {
  config_t config = {0};
  const uint8_t prefix[] = {
    9U, 0x02U, 0U, 0U, 2U, 1U, 0U, 0x80U, 50U,
    9U, 0x04U, 0U, 0U, 0U, 0x01U, 0x01U, 0U, 0U,
    9U, 0x24U, 0x01U, 0U, 1U, 9U, 0U, 1U, 1U
  };
  append(&config, prefix, sizeof(prefix));
  config.midi_interface = config.length;
  const uint8_t interface[] = {9U, 0x04U, 1U, 0U, 2U, 0x01U, 0x03U, 0U, 0U};
  append(&config, interface, sizeof(interface));
  config.header = config.length;
  const uint8_t header[] = {7U, 0x24U, 0x01U, 0U, 1U, 0U, 0U};
  append(&config, header, sizeof(header));
  config.jacks = config.length;
  unsigned int cables = in_cables > out_cables ? in_cables : out_cables;
  for (unsigned int i = 0U; i < cables; i++) {
    uint8_t base = (uint8_t)(4U * i);
    const uint8_t jacks[] = {
      6U, 0x24U, 0x02U, 1U, (uint8_t)(base + 1U), 0U,
      6U, 0x24U, 0x02U, 2U, (uint8_t)(base + 2U), 0U,
      9U, 0x24U, 0x03U, 1U, (uint8_t)(base + 3U), 1U,
          (uint8_t)(base + 2U), 1U, 0U,
      9U, 0x24U, 0x03U, 2U, (uint8_t)(base + 4U), 1U,
          (uint8_t)(base + 1U), 1U, 0U
    };
    append(&config, jacks, sizeof(jacks));
  }
  assert(endpoint_size == 7U || endpoint_size == 9U);
  config.out_endpoint = config.length;
  const uint8_t out[] = {(uint8_t)endpoint_size, 0x05U, 0x02U, 2U, 64U, 0U, 0U, 0U, 0U};
  append(&config, out, endpoint_size);
  config.out_association = config.length;
  association(&config, out_cables, false);
  config.in_endpoint = config.length;
  const uint8_t in[] = {(uint8_t)endpoint_size, 0x05U, 0x81U, 2U, 64U, 0U, 0U, 0U, 0U};
  append(&config, in, endpoint_size);
  config.in_association = config.length;
  association(&config, in_cables, true);
  unsigned int midi_length = config.length - config.header;
  config.data[config.header + 5U] = (uint8_t)midi_length;
  config.data[config.header + 6U] = (uint8_t)(midi_length >> 8U);
  set_length(&config);
  return config;
}

static void reject(const config_t *config) {
  usb_midi_config_t parsed = {0};
  assert(!usb_midi_parse_config(config->data, config->length, &parsed));
}

static void erase(config_t *config, unsigned int offset, unsigned int count) {
  assert(offset + count <= config->length);
  memmove(config->data + offset, config->data + offset + count,
      config->length - offset - count);
  config->length -= count;
  set_length(config);
}

static void insert(config_t *config, unsigned int offset,
    const uint8_t *bytes, unsigned int count) {
  assert(offset <= config->length && config->length + count <= sizeof(config->data));
  memmove(config->data + offset + count, config->data + offset, config->length - offset);
  memcpy(config->data + offset, bytes, count);
  config->length += count;
  set_length(config);
}

static void test_valid(void) {
  const unsigned int counts[][2] = {{1U, 1U}, {2U, 1U}, {1U, 2U},
    {USB_MIDI_MAX_CABLES, USB_MIDI_MAX_CABLES}};
  for (unsigned int size = 7U; size <= 9U; size += 2U) {
    for (unsigned int i = 0U; i < sizeof(counts) / sizeof(counts[0]); i++) {
      config_t config = configuration(counts[i][0], counts[i][1], size);
      usb_midi_config_t parsed = {0};
      assert(usb_midi_parse_config(config.data, config.length, &parsed));
      assert(parsed.configuration == 1U && parsed.midi_interface == 1U);
      assert(parsed.midi_alternate == 0U && parsed.midi_found);
      assert(parsed.in_found && parsed.out_found);
      assert(parsed.bulk_in == 0x81U && parsed.bulk_out == 0x02U);
      assert(parsed.bulk_in_mps == 64U && parsed.bulk_out_mps == 64U);
      assert(parsed.bulk_in_jacks == counts[i][0]);
      assert(parsed.bulk_out_jacks == counts[i][1]);
    }
  }
}

static void test_associations(void) {
  config_t valid = configuration(1U, 1U, 9U);
  config_t bad = valid;
  erase(&bad, valid.in_association, valid.data[valid.in_association]);
  reject(&bad);
  bad = valid;
  erase(&bad, valid.out_association, valid.data[valid.out_association]);
  reject(&bad);
  bad = valid;
  erase(&bad, valid.in_association, valid.data[valid.in_association]);
  erase(&bad, valid.out_association, valid.data[valid.out_association]);
  reject(&bad);

  // Both directions are still mandatory, even if the remaining endpoint
  // has valid jack associations. Do not broaden single-direction support.
  bad = valid;
  erase(&bad, valid.in_endpoint, valid.length - valid.in_endpoint);
  reject(&bad);
  bad = valid;
  erase(&bad, valid.out_endpoint, valid.in_endpoint - valid.out_endpoint);
  reject(&bad);

  for (unsigned int direction = 0U; direction < 2U; direction++) {
    bad = configuration(direction == 0U ? 0U : 1U, direction == 0U ? 1U : 0U, 9U);
    reject(&bad); // Well-framed, zero-cable association.
    bad = configuration(direction == 0U ? USB_MIDI_MAX_CABLES + 1U : 1U,
        direction == 0U ? 1U : USB_MIDI_MAX_CABLES + 1U, 9U);
    reject(&bad); // Well-framed, unsupported cable count.
    unsigned int offset = direction == 0U ? valid.in_association : valid.out_association;
    const uint8_t malformed_counts[] = {0U, 2U, 17U, 255U};
    for (unsigned int i = 0U; i < sizeof(malformed_counts); i++) {
      bad = valid;
      bad.data[offset + 3U] = malformed_counts[i];
      reject(&bad);
    }
    bad = valid;
    bad.data[offset + 2U] = 2U; // Unsupported subtype.
    reject(&bad);
    bad = valid;
    // A descriptor may not contain extra bytes beyond its advertised IDs.
    const uint8_t extra = 0U;
    insert(&bad, offset + 5U, &extra, 1U);
    bad.data[offset]++;
    reject(&bad);
  }

  const uint8_t association[] = {5U, 0x25U, 1U, 1U, 1U};
  bad = valid;
  insert(&bad, valid.out_endpoint, association, sizeof(association));
  reject(&bad); // Orphan association before any endpoint.
  bad = valid;
  insert(&bad, valid.in_endpoint, association, sizeof(association));
  reject(&bad); // Duplicate OUT association.
  bad = valid;
  append(&bad, association, sizeof(association));
  set_length(&bad);
  reject(&bad); // Duplicate IN association at the configuration boundary.
  bad = valid;
  const uint8_t other_interface[] = {9U, 4U, 2U, 0U, 0U, 0xffU, 0U, 0U, 0U};
  insert(&bad, valid.in_association, other_interface, sizeof(other_interface));
  reject(&bad); // An association in another interface cannot satisfy IN.
  bad = valid;
  const uint8_t unrelated[] = {3U, 0x24U, 0x7fU};
  insert(&bad, valid.in_association, unrelated, sizeof(unrelated));
  reject(&bad); // Association must belong to the immediately preceding EP.
}

static void test_boundaries(void) {
  config_t valid = configuration(1U, 1U, 9U);
  usb_midi_config_t parsed = {0};
  assert(!usb_midi_parse_config(NULL, valid.length, &parsed));
  assert(!usb_midi_parse_config(valid.data, valid.length, NULL));
  // Exact-sized allocations make sanitizer builds detect even one-byte
  // reads beyond the supplied configuration, independently of wTotalLength.
  for (unsigned int length = 0U; length < valid.length; length++) {
    uint8_t *prefix = malloc(length == 0U ? 1U : length);
    assert(prefix);
    memcpy(prefix, valid.data, length);
    if (length >= 4U) {
      prefix[2] = (uint8_t)length;
      prefix[3] = (uint8_t)(length >> 8U);
    }
    assert(!usb_midi_parse_config(prefix, length, &parsed));
    free(prefix);
  }
  config_t bad = valid;
  bad.data[0] = 8U;
  reject(&bad);
  bad = valid;
  bad.data[1] = 1U;
  reject(&bad);
  bad = valid;
  bad.data[2]--;
  reject(&bad);
  bad = valid;
  bad.data[2]++;
  reject(&bad);

  // Invalid bLength values at every descriptor boundary must never hang,
  // cross wTotalLength, or let a following descriptor satisfy an endpoint.
  for (unsigned int offset = 9U; offset < valid.length; offset += valid.data[offset]) {
    const uint8_t sizes[] = {0U, 1U, 255U};
    for (unsigned int i = 0U; i < sizeof(sizes); i++) {
      bad = valid;
      bad.data[offset] = sizes[i];
      reject(&bad);
    }
  }
  const unsigned int offsets[] = {valid.midi_interface, valid.header, valid.jacks,
    valid.jacks + 12U, valid.out_endpoint, valid.out_association, valid.in_association};
  const uint8_t short_sizes[] = {8U, 6U, 5U, 6U, 6U, 3U, 3U};
  for (unsigned int i = 0U; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
    bad = valid;
    bad.data[offsets[i]] = short_sizes[i];
    reject(&bad);
  }
  bad = valid;
  bad.data[valid.jacks + 12U + 5U] = 2U; // Two OUT-jack pins need eleven bytes.
  reject(&bad);
  bad = valid;
  const uint8_t trailing = 0U;
  append(&bad, &trailing, 1U);
  set_length(&bad);
  reject(&bad); // Both associations valid, but trailing descriptor truncated.
}

int main(void) {
  test_valid();
  test_associations();
  test_boundaries();
  puts("USB MIDI configuration descriptors: PASS");
  return 0;
}
