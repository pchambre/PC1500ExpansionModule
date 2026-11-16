import 'package:flutter_test/flutter_test.dart';
import 'package:pc1500_ble/link.dart';

void main() {
  // 10 PRINT "A" / 20 END, as BASIC holds it: [no hi][lo][len][... CR], FF.
  const basic = [0x00, 0x0A, 0x05, 0xF0, 0x97, 0x22, 0x41, 0x0D, 0x00, 0x14, 0x03, 0xF1, 0x8E, 0x0D, 0xFF];

  test('a BASIC program is BASIC', () {
    expect(fileKind(basic), kindBasic);
    expect(fileKind(const [0xFF]), kindBasic); // no lines
    expect(fileKind(const []), kindBasic); // empty
  });

  test('an M file is M', () {
    expect(fileKind(const [0x40, 0xC5, 0x00, 0x00, 1, 2, 3]), kindM); // [load][call] + bytes
    expect(fileKind(const [0xFF, 0x42, 0x40, 0xC5, 1, 0xFF, 0x40, 0xC5, 0, 1, 9]), kindM); // banked
    expect(fileKind(const [0x00, 0x0A, 0x02, 0x41, 0x0D, 0xFF, 0x00]), kindM); // bytes after the FF
    expect(fileKind(basic.sublist(0, basic.length - 1)), kindM); // no FF
    expect(fileKind(const [0x00, 0x14, 0x02, 0x41, 0x0D, 0x00, 0x0A, 0x02, 0x41, 0x0D, 0xFF]), kindM); // falling
    expect(fileKind(const [0x00, 0x0A, 0x02, 0x41, 0x42, 0xFF]), kindM); // no CR
  });
}
