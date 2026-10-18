import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:pc1500_ble/screen.dart';

Uint8List rec(int kind, List<int> bytes) => Uint8List.fromList([kind, bytes.length, ...bytes]);

void main() {
  test('DISPLAY: an edit line, then the line it became, then PRINT output', () {
    final s = ScreenModel();
    expect(
      s.add(
        Uint8List.fromList([
          ...rec(3, [5, ...latin1.encode('PRINT')]),
        ]),
      ),
      isTrue,
    );
    expect(s.edit, 'PRINT');
    expect(s.cursor, 5);
    expect(s.add(Uint8List.fromList([...rec(1, latin1.encode('PRINT "HI"')), ...rec(2, latin1.encode('HI'))])), isTrue);
    expect(s.lines.map((l) => (l.kind, l.text)), [(1, 'PRINT "HI"'), (2, 'HI')]);
    expect(s.edit, isNull);
    expect(s.add(rec(3, [0xFF, ...latin1.encode('RUN')])), isTrue);
    expect(s.cursor, -1);
  });

  test('DISPLAY: a record running past the payload is refused, none of it shown', () {
    final s = ScreenModel();
    expect(s.add(Uint8List.fromList([1, 2, 0x41, 2, 9, 0x42])), isFalse);
    expect(s.lines, isEmpty);
  });

  test('DISPLAY: CLEAR empties the screen; what follows it in the frame stays', () {
    final s = ScreenModel();
    s.add(rec(1, latin1.encode('10 PRINT "HI"')));
    expect(s.add(Uint8List.fromList([...rec(4, []), ...rec(2, latin1.encode('AFTER'))])), isTrue);
    expect(s.lines.single.text, 'AFTER');
  });

  test('DISPLAY: STATUS keeps the two indicator bytes, and nothing else changes', () {
    final s = ScreenModel();
    s.add(rec(1, latin1.encode('10 END')));
    expect(s.add(rec(5, [0x41, 0x60])), isTrue);
    expect((s.status1, s.status2), (0x41, 0x60));
    expect(s.lines.single.text, '10 END');
    expect(s.add(rec(5, [1])), isFalse);
  });

  test('DISPLAY: an unknown kind is skipped', () {
    final s = ScreenModel();
    expect(
      s.add(
        Uint8List.fromList([
          ...rec(9, [1, 2]),
          ...rec(1, latin1.encode('10 END')),
        ]),
      ),
      isTrue,
    );
    expect(s.lines.single.text, '10 END');
  });
}
