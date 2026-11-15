import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

final cloudSession = CloudSession();

class CloudSession extends ChangeNotifier {
  String baseUrl = '';
  String? _token;
  bool get signedIn => _token != null;
  bool _loaded = false;

  Future<void> loadConfig() async {
    if (_loaded) return;
    final config = jsonDecode(await rootBundle.loadString('assets/cloud.json'));
    if (baseUrl.isEmpty) baseUrl = config['baseUrl'] as String? ?? '';
    _loaded = true;
  }

  static String validateUrl(String input) {
    final uri = Uri.tryParse(input.trim());
    if (uri == null ||
        uri.scheme != 'https' ||
        uri.host.isEmpty ||
        uri.userInfo.isNotEmpty ||
        uri.hasQuery ||
        uri.hasFragment ||
        (uri.path.isNotEmpty && uri.path != '/')) {
      throw const FormatException(
        'Укажи HTTPS-адрес облака, например https://home.example.ru',
      );
    }
    return uri.replace(path: '').toString();
  }

  Future<void> login(
    String address,
    String email,
    String password, {
    bool register = false,
  }) async {
    baseUrl = validateUrl(address);
    _token = null;
    final response = await request(
      'POST',
      '/api/auth/${register ? 'register' : 'login'}',
      {'email': email.trim(), 'password': password},
    );
    _token = response['token'] as String;
    notifyListeners();
  }

  Future<void> logout() async {
    try {
      await request('POST', '/api/auth/logout', {});
    } finally {
      _token = null;
      notifyListeners();
    }
  }

  Future<dynamic> request(String method, String path, [Object? body]) async {
    if (baseUrl.isEmpty) {
      throw const FormatException('Сначала укажи адрес облака');
    }
    final client = HttpClient()
      ..connectionTimeout = const Duration(seconds: 10);
    try {
      return await (() async {
        final request = await client.openUrl(
          method,
          Uri.parse('$baseUrl$path'),
        );
        request.followRedirects = false;
        if (_token != null) {
          request.headers.set('Authorization', 'Bearer $_token');
        }
        if (method == 'POST') {
          final bytes = utf8.encode(jsonEncode(body ?? {}));
          request.headers.contentType = ContentType.json;
          request.contentLength = bytes.length;
          request.add(bytes);
        }
        final response = await request.close();
        final bytes = <int>[];
        await for (final chunk in response) {
          bytes.addAll(chunk);
          if (bytes.length > 128 * 1024) {
            throw const FormatException('Ответ облака слишком большой');
          }
        }
        if (response.statusCode < 200 || response.statusCode >= 300) {
          if (response.statusCode == 401) {
            _token = null;
            notifyListeners();
          }
          throw CloudException(switch (response.statusCode) {
            401 => 'Войди в аккаунт заново или проверь пароль.',
            403 => 'Регистрация отключена администратором облака.',
            404 => 'Устройство не найдено в твоём аккаунте.',
            409 => 'Действие недоступно: проверь подключение устройства или существующий аккаунт.',
            429 => 'Слишком много запросов. Подожди минуту.',
            400 => 'Проверь поля. Пароль аккаунта — от 12 символов, до 72 байт UTF-8.',
            _ => 'Облако вернуло ошибку ${response.statusCode}.',
          });
        }
        return bytes.isEmpty ? null : jsonDecode(utf8.decode(bytes));
      })().timeout(const Duration(seconds: 15));
    } finally {
      client.close(force: true);
    }
  }
}

class CloudException implements Exception {
  const CloudException(this.message);
  final String message;
  @override
  String toString() => message;
}

String cloudError(Object error) => error is CloudException
    ? error.message
    : error is FormatException
    ? error.message
    : 'Нет связи с облаком. Проверь интернет и адрес сервера.';
