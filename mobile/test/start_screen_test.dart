import 'package:flutter_test/flutter_test.dart';
import 'package:smart_security/main.dart';

void main() {
  testWidgets('starts with Bluetooth discovery and an empty event list', (
    tester,
  ) async {
    await tester.pumpWidget(const SecurityApp());
    expect(find.text('Найти ESP32'), findsOneWidget);
    expect(find.text('Пока нет событий'), findsOneWidget);
    expect(find.text('Сирена'), findsNothing);
  });
}
