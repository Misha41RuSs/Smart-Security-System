import 'dart:async';

import 'package:material_ui/material_ui.dart';

import 'cloud_api.dart';
import 'security_protocol.dart';

class CloudPage extends StatefulWidget {
  const CloudPage({super.key});
  @override
  State<CloudPage> createState() => _CloudPageState();
}

class _CloudPageState extends State<CloudPage> {
  final address = TextEditingController();
  final email = TextEditingController();
  final password = TextEditingController();
  bool busy = false, register = false;
  String? message;
  List<dynamic> devices = [];

  @override
  void initState() {
    super.initState();
    cloudSession.loadConfig().then((_) {
      if (mounted) setState(() => address.text = cloudSession.baseUrl);
    });
    if (cloudSession.signedIn) unawaited(refresh());
  }

  @override
  void dispose() {
    address.dispose();
    email.dispose();
    password.dispose();
    super.dispose();
  }

  Future<void> refresh() async {
    setState(() {
      busy = true;
      message = null;
    });
    try {
      final result = await cloudSession.request('GET', '/api/devices');
      if (mounted) setState(() => devices = result as List);
    } catch (e) {
      if (mounted) setState(() => message = cloudError(e));
    } finally {
      if (mounted) setState(() => busy = false);
    }
  }

  Future<void> signIn() async {
    setState(() {
      busy = true;
      message = null;
    });
    try {
      await cloudSession.login(
        address.text,
        email.text,
        password.text,
        register: register,
      );
      if (mounted) {
        password.clear();
        await refresh();
      }
    } catch (e) {
      if (mounted) setState(() => message = cloudError(e));
    } finally {
      if (mounted) setState(() => busy = false);
    }
  }

  @override
  Widget build(BuildContext context) => Scaffold(
    appBar: AppBar(title: const Text('Мой дом · Облако')),
    body: ListView(
      padding: const EdgeInsets.all(16),
      children: [
        if (message != null)
          Padding(padding: const EdgeInsets.all(8), child: Text(message!)),
        if (!cloudSession.signedIn) ...[
          const Text(
            'Войди для удалённого просмотра и управления. Локальное подключение по Bluetooth работает отдельно.',
          ),
          const SizedBox(height: 16),
          TextField(
            controller: address,
            decoration: const InputDecoration(
              labelText: 'Адрес облака',
              hintText: 'https://home.example.ru',
            ),
            keyboardType: TextInputType.url,
            autocorrect: false,
          ),
          TextField(
            controller: email,
            decoration: const InputDecoration(labelText: 'Email'),
            keyboardType: TextInputType.emailAddress,
            autocorrect: false,
          ),
          TextField(
            controller: password,
            decoration: const InputDecoration(
              labelText: 'Пароль аккаунта (от 12 символов)',
            ),
            obscureText: true,
          ),
          SwitchListTile(
            title: const Text('Создать аккаунт'),
            value: register,
            onChanged: busy ? null : (v) => setState(() => register = v),
          ),
          FilledButton(
            onPressed: busy ? null : signIn,
            child: Text(register ? 'Зарегистрироваться' : 'Войти'),
          ),
        ] else ...[
          const Text(
            'Показания передаются, пока открыт экран устройства. Тревоги сохраняются в облаке независимо от просмотра.',
          ),
          const SizedBox(height: 12),
          if (devices.isEmpty && !busy)
            const Text(
              'Пока нет устройств. Подключись к ESP32 по Bluetooth и открой «Настроить Wi-Fi».',
            ),
          for (final d in devices)
            Card(
              child: ListTile(
                leading: Icon(
                  d['role'] == 'CLIMATE'
                      ? Icons.thermostat
                      : Icons.shield_outlined,
                ),
                title: Text(d['name'] as String),
                subtitle: Text(
                  d['role'] == 'CLIMATE'
                      ? 'Климатический узел'
                      : 'Охранный узел',
                ),
                trailing: const Icon(Icons.chevron_right),
                onTap: () async {
                  await Navigator.of(context).push(
                    MaterialPageRoute<void>(
                      builder: (_) =>
                          CloudDevicePage(device: Map<String, dynamic>.from(d)),
                    ),
                  );
                  if (mounted) await refresh();
                },
              ),
            ),
          TextButton.icon(
            onPressed: busy ? null : refresh,
            icon: const Icon(Icons.refresh),
            label: const Text('Обновить список'),
          ),
          TextButton(
            onPressed: busy
                ? null
                : () async {
                    try {
                      await cloudSession.logout();
                    } catch (_) {
                      /* The local session is cleared even while offline. */
                    }
                    if (mounted) setState(() => devices = []);
                  },
            child: const Text('Выйти из аккаунта'),
          ),
        ],
        if (busy) const LinearProgressIndicator(),
      ],
    ),
  );
}

class CloudDevicePage extends StatefulWidget {
  const CloudDevicePage({required this.device, super.key});
  final Map<String, dynamic> device;
  @override
  State<CloudDevicePage> createState() => _CloudDevicePageState();
}

class _CloudDevicePageState extends State<CloudDevicePage>
    with WidgetsBindingObserver {
  Timer? timer;
  bool active = true, fetching = false, sending = false;
  Map<String, dynamic>? snapshot;
  String? message;
  String get path => '/api/devices/${widget.device['id']}';
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    unawaited(refresh());
    timer = Timer.periodic(const Duration(seconds: 3), (_) {
      if (active) unawaited(refresh());
    });
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    active = state == AppLifecycleState.resumed;
    if (active) unawaited(refresh());
  }

  @override
  void dispose() {
    active = false;
    timer?.cancel();
    WidgetsBinding.instance.removeObserver(this);
    super.dispose();
  }

  Future<void> refresh() async {
    if (!active || fetching || !cloudSession.signedIn) return;
    fetching = true;
    try {
      final value = await cloudSession.request('POST', '$path/live', {});
      if (mounted) {
        setState(() {
          snapshot = Map<String, dynamic>.from(value);
          message = null;
        });
      }
    } catch (e) {
      if (mounted) setState(() => message = cloudError(e));
    } finally {
      fetching = false;
    }
  }

  Future<void> command(SecurityCommand command) async {
    setState(() {
      sending = true;
      message = null;
    });
    try {
      await cloudSession.request('POST', '$path/commands', {
        'code': command.code,
      });
      if (mounted) {
        setState(() => message = 'Команда ожидает подтверждения устройства.');
        await refresh();
      }
    } catch (e) {
      if (mounted) setState(() => message = cloudError(e));
    } finally {
      if (mounted) setState(() => sending = false);
    }
  }

  String reading(double? value, String unit) =>
      value == null ? '—' : '${value.toStringAsFixed(1)} $unit';
  String date(dynamic ms) => ms is num
      ? DateTime.fromMillisecondsSinceEpoch(ms.toInt())
            .toLocal()
            .toString()
            .split('.')
            .first
      : 'Время устройства неизвестно';

  @override
  Widget build(BuildContext context) {
    SecurityState? state;
    final hex = snapshot?['packet'] as String?;
    if (hex != null) {
      state = SecurityState.decode([
        for (int i = 0; i < hex.length; i += 2)
          int.parse(hex.substring(i, i + 2), radix: 16),
      ]);
    }
    final online = snapshot?['online'] == true && message == null;
    final commands = snapshot?['commands'] as List? ?? [];
    final pending = commands.any((c) => c['status'] == 'PENDING');
    final canCommand = online && !sending && !pending;
    final alerts = snapshot?['alerts'] as List? ?? [];
    return Scaffold(
      appBar: AppBar(title: Text(widget.device['name'] as String)),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          ListTile(
            leading: Icon(online ? Icons.cloud_done : Icons.cloud_off),
            title: Text(
              online ? 'На связи с облаком' : 'Нет подтверждённой связи',
            ),
            subtitle: Text(
              snapshot?['sampled_at'] == null
                  ? 'Ожидаем первый замер'
                  : 'Последний замер: ${date(snapshot!['sampled_at'])}',
            ),
          ),
          if (message != null) Text(message!),
          if (state != null) ...[
            if (!state.climateOnly) ...[
              SwitchListTile(
                title: const Text('Охрана'),
                value: state.armed,
                onChanged: canCommand
                    ? (v) => command(
                        v ? SecurityCommand.arm : SecurityCommand.disarm,
                      )
                    : null,
              ),
              SwitchListTile(
                title: const Text('Сирена'),
                value: state.siren,
                onChanged: canCommand
                    ? (v) => command(
                        v ? SecurityCommand.sirenOn : SecurityCommand.sirenOff,
                      )
                    : null,
              ),
              ListTile(
                title: const Text('Расстояние'),
                trailing: Text(reading(state.distance, 'см')),
              ),
              ListTile(
                title: const Text('Освещённость'),
                trailing: Text(
                  state.light == null ? '—' : '${state.light} АЦП',
                ),
              ),
            ],
            ListTile(
              title: const Text('Температура'),
              trailing: Text(reading(state.temperature, '°C')),
            ),
            ListTile(
              title: const Text('Влажность'),
              trailing: Text(reading(state.humidity, '%')),
            ),
            ListTile(
              title: const Text('Давление'),
              trailing: Text(reading(state.pressure, 'гПа')),
            ),
          ],
          if (commands.isNotEmpty)
            Text(
              'Последняя команда: ${switch (commands.first['status']) {
                'APPLIED' => 'подтверждена платой',
                'REJECTED' => 'отклонена платой',
                'EXPIRED' => 'истёк срок ожидания',
                _ => 'ожидает подтверждения',
              }}',
            ),
          const SizedBox(height: 24),
          Text('История тревог', style: Theme.of(context).textTheme.titleLarge),
          if ((snapshot?['lost_alerts'] as num? ?? 0) > 0)
            Text(
              'При переполнении памяти плата потеряла событий: ${snapshot!['lost_alerts']}',
            ),
          if (alerts.isEmpty) const Text('Пока нет тревог'),
          for (final event in alerts)
            ListTile(
              leading: const Icon(Icons.warning_amber_rounded),
              title: Text(switch (event['kind']) {
                'DISTANCE' => 'Расстояние ниже порога',
                'LIGHT' => 'Обнаружен свет',
                'OVERHEAT' => 'Температура выше порога',
                _ => 'Тревога',
              }),
              subtitle: Text(
                event['occurred_at'] == null
                    ? 'Получено ${date(event['received_at'])} · время события не синхронизировано'
                    : '${date(event['occurred_at'])} · получено ${date(event['received_at'])}',
              ),
            ),
        ],
      ),
    );
  }
}
