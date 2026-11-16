// The host's printer (2026-11-14): BLPRTC prints the console and BLPRTP the
// plotter's paper (RP2350/BLE_PROTOCOL.md, "Paper"). Both are made into a
// PDF and sent to the printer chosen in Settings (the system's default if
// none) with no dialog on desktops; phones can't print without one, so
// there the system's print dialog opens.
import 'dart:io';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:pdf/pdf.dart';
import 'package:pdf/widgets.dart' as pw;
import 'package:printing/printing.dart';

import 'plot.dart';

const _mm = PdfPageFormat.mm;
const _margin = 15 * _mm;

/// Phones print through the system's dialog; desktops straight to a printer.
bool get printsWithDialog => Platform.isAndroid || Platform.isIOS;

/// The printer to use on a desktop: the one Settings names if it's still
/// there, else the system's default; null if there's none.
Future<Printer?> findPrinter(String url) async {
  final printers = await Printing.listPrinters();
  Printer? named, fallback;
  for (final p in printers) {
    if (url.isNotEmpty && p.url == url) named = p;
    if (p.isDefault) fallback ??= p;
  }
  return named ?? fallback;
}

/// The document to `printer` (null: the dialog). True once it's been handed
/// to the system.
Future<bool> printDocument(
  Printer? printer,
  String name,
  Future<Uint8List> Function(PdfPageFormat format) build,
) async {
  if (printer == null) return Printing.layoutPdf(name: name, onLayout: build);
  return await Printing.directPrintPdf(printer: printer, name: name, onLayout: build);
}

/// The console's text in Courier, as many pages as it takes.
Future<Uint8List> consolePdf(String text, PdfPageFormat format) {
  final doc = pw.Document(title: 'PC-1500 console');
  final style = pw.TextStyle(font: pw.Font.courier(), fontSize: 10);
  final lines = text.split('\n');
  if (lines.isNotEmpty && lines.last.isEmpty) lines.removeLast();
  doc.addPage(pw.MultiPage(
    pageFormat: format,
    margin: const pw.EdgeInsets.all(_margin),
    build: (_) => [for (final l in lines) pw.Text(_printable(l), style: style)],
  ));
  return doc.save();
}

/// Courier has Latin-1's printable characters; anything else is '?'. An
/// empty line keeps its height as a space.
String _printable(String line) {
  if (line.isEmpty) return ' ';
  return String.fromCharCodes(line.runes.map((c) => c < 0x20 || (c >= 0x7F && c < 0xA0) || c > 0xFF ? 0x3F : c));
}

/// What was drawn, kept apart from the paper so it can be printed while the
/// PC-1500 goes on drawing.
class PlotSnapshot {
  PlotSnapshot(PlotPaper paper)
      : lines = List.of(paper.lines),
        top = paper.top,
        bottom = paper.bottom;
  final List<PlotLine> lines;
  final int top, bottom;
}

/// The roll down the page, the earliest output at the top, continued on as
/// many pages as it takes. `fit`: scaled to the page's width, else actual
/// size (the roll 58 mm wide, the pen's travel 43.2 mm).
Future<Uint8List> plotPdf(PlotSnapshot plot, PdfPageFormat format, {required bool fit}) {
  final doc = pw.Document(title: 'PC-1500 plotter');
  const q = _mm / 20; // a quarter step, in points
  const leadQ = 5.0 * 20; // 5 mm of blank roll before and after
  final width = format.width - 2 * _margin, height = format.height - 2 * _margin;
  final s = fit ? width / PaperPainter.paperQ : q; // points per quarter step
  final rollW = PaperPainter.paperQ * s;
  final first = plot.top + leadQ; // the roll's y at the first page's top
  final length = plot.top - plot.bottom + 2 * leadQ;
  final perPage = height / s;
  final pages = math.max(1, (length / perPage).ceil());
  final penWidth = 0.3 * _mm * s / q; // a 0.3 mm pen, scaled with the roll
  final colours = [for (final c in PaperPainter.pens) PdfColor.fromInt(c.toARGB32())];
  for (var i = 0; i < pages; i++) {
    final from = first - i * perPage; // this page's top
    final pageLen = math.min(perPage, length - i * perPage);
    final to = from - pageLen;
    doc.addPage(pw.Page(
      pageFormat: format,
      margin: const pw.EdgeInsets.all(_margin),
      build: (_) => pw.Align(
        alignment: pw.Alignment.topCenter,
        child: pw.CustomPaint(
          size: PdfPoint(rollW, pageLen * s),
          painter: (canvas, size) {
            double px(int x) => (PaperPainter.leftQ + x) * s;
            double py(int y) => size.y - (from - y) * s;
            canvas
              ..setStrokeColor(PdfColors.grey400)
              ..setLineWidth(0.3)
              ..drawLine(0, 0, 0, size.y)
              ..drawLine(rollW, 0, rollW, size.y)
              ..strokePath();
            canvas
              ..saveContext()
              ..drawRect(0, 0, size.x, size.y)
              ..clipPath()
              ..setLineCap(PdfLineCap.round)
              ..setLineWidth(penWidth);
            // A pen at a time; a colour can't change in the middle of a path.
            for (var pen = 0; pen < 4; pen++) {
              final mine = plot.lines.where(
                  (l) => (l.pen & 3) == pen && math.max(l.y0, l.y1) >= to && math.min(l.y0, l.y1) <= from);
              if (mine.isEmpty) continue;
              canvas.setStrokeColor(colours[pen]);
              for (final l in mine) {
                canvas
                  ..moveTo(px(l.x0), py(l.y0))
                  ..lineTo(px(l.x1), py(l.y1));
              }
              canvas.strokePath();
            }
            canvas.restoreContext();
          },
        ),
      ),
    ));
  }
  return doc.save();
}
