import 'package:material_ui/material_ui.dart';

import 'ble_security.dart';
import 'security_protocol.dart';

void main() => runApp(const SecurityApp());

class SecurityApp extends StatelessWidget {
  const SecurityApp({super.key});

  @override
  Widget build(BuildContext context) => MaterialApp(
    title: 'Охрана ESP32',
    debugShowCheckedModeBanner: false,
    theme: ThemeData(
      colorScheme: ColorScheme.fromSeed(
        seedColor: const Color(0xff286458),
        brightness: Brightness.dark,
      ),
    ),
    home: const SecurityPage(),
  );
}

class SecurityPage extends StatefulWidget {
  const SecurityPage({super.key});

  @override
  State<SecurityPage> createState() => _SecurityPageState();
}

class _SecurityPageState extends State<SecurityPage> {
  final _security = BleSecurity();

  @override
  void dispose() {
    _security.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Охрана ESP32')),
      body: SafeArea(
        child: Center(
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 680),
            child: ListenableBuilder(
              listenable: _security,
              builder: (context, child) {
                final state = _security.state;
                final message = _security.message;
                return ListView(
                  padding: const EdgeInsets.fromLTRB(16, 8, 16, 28),
                  children: [
                    _connectionCard(context),
                    if (message != null)
                      Padding(
                        padding: const EdgeInsets.all(12),
                        child: Text(
                          message,
                          style: TextStyle(
                            color: Theme.of(context).colorScheme.error,
                          ),
                        ),
                      ),
                    if (state != null) ...[
                      const SizedBox(height: 16),
                      if (!state.climateOnly) _controls(state),
                      const SizedBox(height: 24),
                      Text(
                        'Датчики',
                        style: Theme.of(context).textTheme.titleLarge,
                      ),
                      const SizedBox(height: 8),
                      _measurements(state),
                    ],
                    const SizedBox(height: 24),
                    if (state?.climateOnly != true) _eventList(context),
                  ],
                );
              },
            ),
          ),
        ),
      ),
    );
  }

  Widget _connectionCard(BuildContext context) {
    final device = _security.device;
    final String title;
    final String description;
    if (_security.ready) {
      title = device!.name.isEmpty ? 'ESP32' : device.name;
      description = _security.state?.climateOnly == true
          ? 'Климатический узел · Подключено по Bluetooth'
          : 'Охранный узел · Подключено по Bluetooth';
    } else if (device != null) {
      title = 'Подключение к ESP32';
      description = 'При первом подключении введи PIN из Serial Monitor.';
    } else if (_security.scanning) {
      title = 'Поиск ESP32';
      description = 'Выбери плату из списка.';
    } else {
      title = 'Подключи ESP32';
      description = 'Включи плату и держи телефон рядом.';
    }
    return Card(
      clipBehavior: Clip.antiAlias,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          ListTile(
            leading: Icon(
              _security.ready ? Icons.bluetooth_connected : Icons.bluetooth,
            ),
            title: Text(title),
            subtitle: Text(description),
          ),
          if (_security.scanning || (device != null && !_security.ready))
            const LinearProgressIndicator(),
          if (device == null)
            for (final found in _security.devices)
              ListTile(
                leading: const Icon(Icons.memory),
                title: Text(found.name.isEmpty ? 'ESP32-SS' : found.name),
                trailing: const Icon(Icons.chevron_right),
                onTap: () => _security.connect(found),
              ),
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 4, 16, 12),
            child: device != null
                ? TextButton(
                    onPressed: _security.disconnect,
                    child: const Text('Отключиться'),
                  )
                : FilledButton.tonalIcon(
                    onPressed: _security.scanning ? null : _security.scan,
                    icon: const Icon(Icons.search),
                    label: const Text('Найти ESP32'),
                  ),
          ),
        ],
      ),
    );
  }

  Widget _controls(SecurityState state) => Card(
    child: Column(
      children: [
        SwitchListTile(
          secondary: const Icon(Icons.shield_outlined),
          title: const Text('Охрана'),
          subtitle: Text(state.armed ? 'Под охраной' : 'Снято с охраны'),
          value: state.armed,
          onChanged: _security.writing
              ? null
              : (on) => _security.send(
                  on ? SecurityCommand.arm : SecurityCommand.disarm,
                ),
        ),
        const Divider(height: 1, indent: 16, endIndent: 16),
        SwitchListTile(
          secondary: const Icon(Icons.campaign_outlined),
          title: const Text('Сирена'),
          value: state.siren,
          onChanged: _security.writing
              ? null
              : (on) => _security.send(
                  on ? SecurityCommand.sirenOn : SecurityCommand.sirenOff,
                ),
        ),
      ],
    ),
  );

  Widget _measurements(SecurityState state) {
    final metrics = [
      if (!state.climateOnly)
        (
          label: 'Расстояние',
          value: _reading(state.distance, 'см'),
          icon: Icons.straighten,
          alert: state.isActive(SecurityAlert.distance),
        ),
      if (!state.climateOnly)
        (
          label: 'Освещённость',
          value: state.light == null ? '—' : '${state.light} АЦП',
          icon: Icons.light_mode_outlined,
          alert: state.isActive(SecurityAlert.light),
        ),
      (
        label: 'Температура',
        value: _reading(state.temperature, '°C'),
        icon: Icons.thermostat,
        alert: state.isActive(SecurityAlert.temperature),
      ),
      (
        label: 'Влажность',
        value: _reading(state.humidity, '%'),
        icon: Icons.water_drop_outlined,
        alert: false,
      ),
      (
        label: 'Давление',
        value: _reading(state.pressure, 'гПа'),
        icon: Icons.speed,
        alert: false,
      ),
    ];
    return LayoutBuilder(
      builder: (context, constraints) {
        final columns = switch (constraints.maxWidth) {
          >= 560 => 3,
          >= 300 => 2,
          _ => 1,
        };
        final width = (constraints.maxWidth - 8 * (columns - 1)) / columns;
        final colors = Theme.of(context).colorScheme;
        return Wrap(
          spacing: 8,
          runSpacing: 8,
          children: [
            for (final metric in metrics)
              SizedBox(
                width: width,
                child: Card(
                  margin: EdgeInsets.zero,
                  color: metric.alert ? colors.errorContainer : null,
                  child: Padding(
                    padding: const EdgeInsets.all(12),
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Icon(metric.icon, size: 20),
                        const SizedBox(height: 8),
                        Text(metric.label),
                        Text(
                          metric.value,
                          style: Theme.of(context).textTheme.titleLarge,
                        ),
                      ],
                    ),
                  ),
                ),
              ),
          ],
        );
      },
    );
  }

  String _reading(double? value, String unit) =>
      value == null ? '—' : '${value.toStringAsFixed(1)} $unit';

  Widget _eventList(BuildContext context) {
    final events = _security.events;
    final notificationStatus = _security.notificationStatus;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text('События охраны', style: Theme.of(context).textTheme.titleLarge),
        TextButton.icon(
          onPressed: _security.settingNotifications
              ? null
              : _security.requestNotifications,
          icon: const Icon(Icons.notifications_outlined),
          label: const Text('Разрешить уведомления телефона'),
        ),
        if (notificationStatus != null) Text(notificationStatus),
        if (events.isEmpty) const ListTile(title: Text('Пока нет событий')),
        for (final event in events)
          ListTile(
            leading: const Icon(Icons.warning_amber_rounded),
            title: Text(event.alert.title),
            trailing: Text(
              MaterialLocalizations.of(context)
                  .formatTimeOfDay(TimeOfDay.fromDateTime(event.time)),
            ),
          ),
      ],
    );
  }
}
