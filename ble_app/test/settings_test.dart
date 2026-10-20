import 'package:flutter_test/flutter_test.dart';
import 'package:pc1500_ble/settings.dart';

void main() {
  test('a name is 1 to 16 printable Latin-1 characters', () {
    expect(Settings.checkName('PC1500-SRV'), isNull);
    expect(Settings.checkName('Café 1500'), isNull);
    expect(Settings.checkName('1234567890123456'), isNull);
    expect(Settings.checkName(''), isNotNull);
    expect(Settings.checkName('12345678901234567'), isNotNull);
    expect(Settings.checkName('tab\there'), isNotNull);
    expect(Settings.checkName('Ω'), isNotNull);
  });
}
