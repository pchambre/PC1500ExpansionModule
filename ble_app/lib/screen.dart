// The PC-1500's screen text (BLE_PROTOCOL.md's DISPLAY, 2026-10-17, a
// trial): a dongle built with PC1500_TEXT_SNOOP sends the lines its LCD
// shows -- entered, listed, printed -- and the line being typed or edited.
// Shown here as a console with scrollback, like the IWS video interface's
// screen, 80 columns wide.
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/material.dart';

/// DISPLAY record kinds.
class ScreenKind {
  static const line = 1, output = 2, edit = 3, clear = 4; // clear: BLCLS, or CLS typed
  static const status = 5; // the LCD's indicators: [764EH][764FH]
}

class ScreenLine {
  const ScreenLine(this.kind, this.text);
  final int kind;
  final String text;
}

/// Finished lines (at most [maxLines]) and the line being edited now.
class ScreenModel extends ChangeNotifier {
  static const maxLines = 5000;
  final lines = <ScreenLine>[];

  /// The line being typed or edited, or null; its cursor's LCD column, or -1.
  String? edit;
  int cursor = -1;

  /// The LCD's indicators as the PC-1500 keeps them (ROM1's STATUS1/2), or
  /// null before the first STATUS: 764EH bit 0 BUSY, 1 SHIFT, 2 KANA, 3 SML,
  /// 4-6 reserve groups III/II/I; 764FH bit 0 DE, 1 G, 2 RAD, 4 RESERVE,
  /// 5 PRO, 6 RUN, 7 DEF.
  int? status1, status2;

  /// A DISPLAY payload; false if malformed (nothing of it is shown then).
  bool add(Uint8List p) {
    final records = <(int, Uint8List)>[];
    var at = 0;
    while (at < p.length) {
      if (at + 2 > p.length || at + 2 + p[at + 1] > p.length) return false;
      records.add((p[at], Uint8List.sublistView(p, at + 2, at + 2 + p[at + 1])));
      at += 2 + p[at + 1];
    }
    for (final (kind, bytes) in records) {
      switch (kind) {
        case ScreenKind.line:
        case ScreenKind.output:
          lines.add(ScreenLine(kind, latin1.decode(bytes, allowInvalid: true)));
          edit = null;
          cursor = -1;
        case ScreenKind.status:
          if (bytes.length < 2) return false;
          status1 = bytes[0];
          status2 = bytes[1];
        case ScreenKind.clear:
          lines.clear();
          edit = null;
          cursor = -1;
        case ScreenKind.edit:
          if (bytes.isEmpty) return false;
          cursor = bytes[0] >= 0x80 ? bytes[0] - 0x100 : bytes[0];
          edit = latin1.decode(bytes.sublist(1), allowInvalid: true);
        default:
          break; // a kind from a newer PC-1500: skipped
      }
    }
    if (lines.length > maxLines) lines.removeRange(0, lines.length - maxLines);
    notifyListeners();
    return true;
  }

  void clear() {
    lines.clear();
    edit = null;
    cursor = -1;
    notifyListeners();
  }
}

/// The screen: finished lines scrolling up, the edit line (with its cursor)
/// at the bottom. Follows the bottom unless scrolled back.
class ScreenView extends StatefulWidget {
  const ScreenView({super.key, required this.screen});
  final ScreenModel screen;

  @override
  State<ScreenView> createState() => _ScreenViewState();
}

class _ScreenViewState extends State<ScreenView> {
  static const _style = TextStyle(fontFamily: 'Consolas', fontSize: 15, height: 1.25, color: Color(0xFFB8F5B0));
  final _scroll = ScrollController();
  bool _follow = true;

  @override
  void initState() {
    super.initState();
    widget.screen.addListener(_changed);
    _scroll.addListener(() {
      if (_scroll.hasClients) {
        _follow = _scroll.position.pixels >= _scroll.position.maxScrollExtent - 4;
      }
    });
  }

  @override
  void dispose() {
    widget.screen.removeListener(_changed);
    _scroll.dispose();
    super.dispose();
  }

  void _changed() {
    setState(() {});
    if (!_follow) return;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (_scroll.hasClients) _scroll.jumpTo(_scroll.position.maxScrollExtent);
    });
  }

  Widget _editLine(String text, int cursor) {
    if (cursor < 0) return Text(text, style: _style);
    final padded = text.padRight(cursor + 1);
    return Text.rich(
      TextSpan(
        style: _style,
        children: [
          TextSpan(text: padded.substring(0, cursor)),
          TextSpan(
            text: padded[cursor],
            style: const TextStyle(backgroundColor: Color(0xFFB8F5B0), color: Color(0xFF0E1A0E)),
          ),
          TextSpan(text: padded.substring(cursor + 1)),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final s = widget.screen;
    final edit = s.edit;
    // Selectable: drag across lines, then Ctrl+C or the context menu's Copy.
    return Container(
      color: const Color(0xFF0E1A0E),
      padding: const EdgeInsets.all(8),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          IndicatorBar(status1: s.status1, status2: s.status2),
          const SizedBox(height: 6),
          Expanded(
            child: SelectionArea(
              child: ListView.builder(
                controller: _scroll,
                itemCount: s.lines.length + (edit != null ? 1 : 0),
                itemBuilder: (_, i) {
                  if (i == s.lines.length) return _editLine(edit!, s.cursor);
                  final line = s.lines[i];
                  return Text(
                    line.text,
                    style: line.kind == ScreenKind.output ? _style.copyWith(color: const Color(0xFFFFE08A)) : _style,
                  );
                },
              ),
            ),
          ),
        ],
      ),
    );
  }
}

/// The LCD's indicator row, laid out as pc1500emu's (src/host/main.cpp,
/// the indicator bar above its dot matrix -- itself matched to a real
/// PC-1500): ten equal slots across the width; the first nine hold BUSY,
/// SHIFT, katakana, SMALL, DEG/RAD/GRAD, RUN, PRO, RESERVE and DEF, each
/// centred in its slot; the tenth holds I, II and III as a tight cluster,
/// one space apart, centred. Only lit indicators are drawn. 11 pt MS Gothic
/// in an 18-pixel bar, as the emulator's.
class IndicatorBar extends StatelessWidget {
  const IndicatorBar({super.key, required this.status1, required this.status2});
  final int? status1, status2;

  static const height = 18.0;
  static const _style = TextStyle(fontFamily: 'MS Gothic', fontSize: 11, color: Color(0xFFB8F5B0), height: 1.0);

  @override
  Widget build(BuildContext context) {
    final s1 = status1 ?? 0, s2 = status2 ?? 0;
    bool b1(int mask) => s1 & mask != 0;
    bool b2(int mask) => s2 & mask != 0;
    // DE+G is DEG, G alone GRAD, RAD RAD -- one word, as the emulator shows it.
    final angle = b2(0x01) && b2(0x02) ? 'DEG' : (b2(0x02) ? 'GRAD' : (b2(0x04) ? 'RAD' : null));
    final slots = <String?>[
      b1(0x01) ? 'BUSY' : null,
      b1(0x02) ? 'SHIFT' : null,
      b1(0x04) ? '\u30AB\u30BF' : null, // katakana ka-ta
      b1(0x08) ? 'SMALL' : null,
      angle,
      b2(0x40) ? 'RUN' : null,
      b2(0x20) ? 'PRO' : null,
      b2(0x10) ? 'RESERVE' : null,
      b1(0x80) ? 'DEF' : null, // 764EH bit 7, as the emulator reads it (ASMADDR says 764FH bit 7)
    ];
    final roman = [if (b1(0x40)) 'I', if (b1(0x20)) 'II', if (b1(0x10)) 'III'].join(' ');
    return SizedBox(
      height: height,
      child: LayoutBuilder(
        builder: (_, box) {
          final slot = box.maxWidth / (slots.length + 1); // +1: the last slot is I/II/III's
          Widget at(int i, String text) => Positioned(
            left: i * slot,
            width: slot,
            top: 0,
            bottom: 0,
            child: Center(child: Text(text, style: _style, maxLines: 1, softWrap: false)),
          );
          return Stack(
            children: [
              for (var i = 0; i < slots.length; i++)
                if (slots[i] != null) at(i, slots[i]!),
              if (roman.isNotEmpty) at(slots.length, roman),
            ],
          );
        },
      ),
    );
  }
}
