import 'package:flutter_test/flutter_test.dart';
import 'package:pc1500_ble/main.dart';

void main() {
  test('Link UUIDs share one base', () {
    expect(linkRxUuid.substring(8), linkServiceUuid.substring(8));
    expect(linkTxUuid.substring(8), linkServiceUuid.substring(8));
  });
}
