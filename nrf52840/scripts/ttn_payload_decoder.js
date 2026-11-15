// TTN uplink payload formatter (JavaScript), verbatim from the
// Things Network console. Maps the 8-byte record payload to the
// fields catcher.py writes to CSV.
function decodeUplink(input) {
  var b = input.bytes;
  if (b.length < 8) return { errors: ["payload too short"] };
  return {
    data: {
      v: b[0],
      tag_id: b[1],
      event_type: b[2],
      flags: b[3],
      sensor1_mv: (b[4] << 8) | b[5],
      sensor2_mv: (b[6] << 8) | b[7]
    }
  };
}
