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

  testWidgets('cloud account screen is separate from local Bluetooth', (
    tester,
  ) async {
    await tester.pumpWidget(const SecurityApp());
    await tester.tap(find.byTooltip('Облако'));
    await tester.pumpAndSettle();
    expect(find.text('Email'), findsOneWidget);
    expect(find.text('Войти'), findsOneWidget);
    expect(find.text('Адрес облака'), findsOneWidget);
    expect(tester.takeException(), isNull);
  });
}
