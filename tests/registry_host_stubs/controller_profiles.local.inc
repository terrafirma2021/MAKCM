{
    "valid-no-hid", "none",
    0x4131, 0x3519, 0x0100,
    makcm_test_descriptor_hash(), 0,
    makcm_test_descriptor_sha(), makcm_test_zero_sha(),
    0, 0x81, 0, 20, 1, 0, CodecFieldEncoding::Int16LE,
    {10, 0, -32768, 32767, 1},
    {12, 0, -32768, 32767, 1},
    false
},
{
    "fake-no-hid", "none",
    0x4131, 0x3519, 0x0100,
    makcm_test_descriptor_hash(), makcm_test_hid_hash(),
    makcm_test_descriptor_sha(), makcm_test_hid_sha(),
    1, 0x82, 0, 20, 1, 0, CodecFieldEncoding::Int16LE,
    {10, 0, -32768, 32767, 1},
    {12, 0, -32768, 32767, 1},
    false
},
{
    "hid-required", "none",
    0x4131, 0x3519, 0x0100,
    makcm_test_descriptor_hash(), makcm_test_hid_hash(),
    makcm_test_descriptor_sha(), makcm_test_hid_sha(),
    2, 0x83, 0, 20, 1, 0, CodecFieldEncoding::Int16LE,
    {10, 0, -32768, 32767, 1},
    {12, 0, -32768, 32767, 1},
    true
},
