// The CE-150 stand-in's paper (2026-09-30): what the PC-1500's PLOT frames
// draw (RP2350/BLE_PROTOCOL.md, "Plotter"), and a view of it. Coordinates
// are quarter steps (0.05 mm): x 0-860 across the pen's 43.2 mm travel, y
// up the paper. The decoder follows the firmware's plot_decode()
// (RP2350/plotter.c).
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:flutter/material.dart';

/// PLOT operations (BLE_PROTOCOL.md).
class PlotOp {
  static const move = 0x01, draw = 0x02, moveRel = 0x03, drawRel = 0x04, pen = 0x05;
}

const plotPrefix = 7; // [pen][x i16][y i32]

class PlotLine {
  const PlotLine(this.pen, this.x0, this.y0, this.x1, this.y1);
  final int pen, x0, y0, x1, y1;

  @override
  String toString() => 'pen $pen ($x0,$y0)-($x1,$y1)';
}

class PlotPaper extends ChangeNotifier {
  final lines = <PlotLine>[];

  /// Where the pen rests, once anything has come.
  bool penKnown = false;
  int pen = 0, x = 0, y = 0;

  /// The y range everything drawn spans, the pen's rest included.
  int top = 0, bottom = 0;

  /// One PLOT payload. False for a malformed one; what it held up to
  /// there is drawn.
  bool add(Uint8List p) {
    if (p.length < plotPrefix) return false;
    final d = ByteData.sublistView(p);
    var pen = p[0], x = d.getInt16(1, Endian.little), y = d.getInt32(3, Endian.little);
    var ok = true;
    var at = plotPrefix;
    while (at < p.length) {
      final op = p[at];
      final size = switch (op) {
        PlotOp.move || PlotOp.draw => 7,
        PlotOp.moveRel || PlotOp.drawRel => 3,
        PlotOp.pen => 2,
        _ => 0,
      };
      if (size == 0 || at + size > p.length) {
        ok = false;
        break;
      }
      var nx = x, ny = y;
      if (op == PlotOp.move || op == PlotOp.draw) {
        nx = d.getInt16(at + 1, Endian.little);
        ny = d.getInt32(at + 3, Endian.little);
      } else if (op == PlotOp.moveRel || op == PlotOp.drawRel) {
        nx = x + d.getInt8(at + 1);
        ny = y + d.getInt8(at + 2);
      } else {
        pen = p[at + 1];
      }
      if (op == PlotOp.draw || op == PlotOp.drawRel) _line(PlotLine(pen, x, y, nx, ny));
      x = nx;
      y = ny;
      at += size;
    }
    this.pen = pen;
    this.x = x;
    this.y = y;
    _span(y);
    notifyListeners();
    return ok;
  }

  void _line(PlotLine l) {
    _span(l.y0);
    _span(l.y1);
    lines.add(l);
  }

  void _span(int v) {
    if (!penKnown) {
      top = bottom = v;
      penKnown = true;
    }
    top = math.max(top, v);
    bottom = math.min(bottom, v);
  }

  void clear() {
    lines.clear();
    penKnown = false;
    notifyListeners();
  }
}

/// The CE-150's 58 mm roll, the pen's travel starting 5 mm in from its left
/// edge, the newest output at the bottom; it follows the output while
/// scrolled to the end.
class PaperView extends StatefulWidget {
  const PaperView({super.key, required this.paper});
  final PlotPaper paper;

  @override
  State<PaperView> createState() => _PaperViewState();
}

class _PaperViewState extends State<PaperView> {
  final _scroll = ScrollController();

  @override
  void initState() {
    super.initState();
    widget.paper.addListener(_changed);
  }

  @override
  void dispose() {
    widget.paper.removeListener(_changed);
    _scroll.dispose();
    super.dispose();
  }

  void _changed() {
    final atEnd = !_scroll.hasClients || _scroll.position.pixels >= _scroll.position.maxScrollExtent - 2;
    setState(() {});
    if (atEnd) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (_scroll.hasClients) _scroll.jumpTo(_scroll.position.maxScrollExtent);
      });
    }
  }

  @override
  Widget build(BuildContext context) => LayoutBuilder(builder: (context, box) {
        final s = box.maxWidth / PaperPainter.paperQ;
        final length = (widget.paper.top - widget.paper.bottom + 2 * PaperPainter.leadQ) * s;
        return Container(
          color: const Color(0xFF5A5A5F),
          child: SingleChildScrollView(
            controller: _scroll,
            child: CustomPaint(
              size: Size(box.maxWidth, math.max(length, box.maxHeight)),
              painter: PaperPainter(widget.paper),
            ),
          ),
        );
      });
}

class PaperPainter extends CustomPainter {
  PaperPainter(this.paper) : super(repaint: paper);
  final PlotPaper paper;

  static const paperQ = 58.0 * 20; // the roll's width in quarter steps (20 per mm)
  static const leftQ = 5.0 * 20; // the left margin
  static const leadQ = 20.0 * 20; // 20 mm of blank paper around what's drawn
  static const pens = [Color(0xFF141414), Color(0xFF1E3CDC), Color(0xFF148C28), Color(0xFFD21E1E)];

  @override
  void paint(Canvas canvas, Size size) {
    final s = size.width / paperQ;
    canvas.drawRect(Offset.zero & size, Paint()..color = const Color(0xFFFCFAF0));
    Offset at(int x, int y) => Offset((leftQ + x) * s, (paper.top - y + leadQ) * s);
    final stroke = Paint()
      ..strokeWidth = math.max(1.0, 0.3 * 20 * s) // a 0.3 mm pen
      ..strokeCap = StrokeCap.round;
    for (final l in paper.lines) {
      stroke.color = pens[l.pen & 3];
      canvas.drawLine(at(l.x0, l.y0), at(l.x1, l.y1), stroke);
    }
    if (paper.penKnown) {
      canvas.drawCircle(
          at(paper.x, paper.y),
          3,
          Paint()
            ..style = PaintingStyle.stroke
            ..color = pens[paper.pen & 3]);
    }
  }

  @override
  bool shouldRepaint(PaperPainter old) => true;
}
