import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:pc1500_ble/hardcopy.dart';
import 'package:pc1500_ble/plot.dart';
import 'package:pdf/pdf.dart';

/// Page objects in a PDF the pdf package wrote (it doesn't compress them).
int pages(Uint8List pdf) => RegExp(r'/Type\s*/Page\b').allMatches(latin1.decode(pdf)).length;

/// A PLOT payload: the pen at (x, y), then one DRAW to (x2, y2).
Uint8List line(int x, int y, int x2, int y2) {
  final b = ByteData(14)
    ..setUint8(0, 0)
    ..setInt16(1, x, Endian.little)
    ..setInt32(3, y, Endian.little)
    ..setUint8(7, PlotOp.draw)
    ..setInt16(8, x2, Endian.little)
    ..setInt32(10, y2, Endian.little);
  return b.buffer.asUint8List();
}

void main() {
  test('the console: a PDF, more pages for more lines', () async {
    final short = await consolePdf('HELLO\n\nA\u0001B\n', PdfPageFormat.a4);
    expect(latin1.decode(short.sublist(0, 5)), '%PDF-');
    expect(pages(short), 1);
    final long = await consolePdf(List.generate(200, (i) => 'LINE $i').join('\n'), PdfPageFormat.a4);
    expect(pages(long), greaterThan(1));
  });

  test('the plotter paper: actual size down the pages, more of them fitted to the width', () async {
    final paper = PlotPaper();
    // 600 mm of roll (20 quarter steps a mm), corner to corner
    expect(paper.add(line(0, 0, 860, -600 * 20)), isTrue);
    final plot = PlotSnapshot(paper);
    // A4 less 15 mm margins: 267 mm a page; 610 mm with the leads is 3 pages
    expect(pages(await plotPdf(plot, PdfPageFormat.a4, fit: false)), 3);
    final fitted = pages(await plotPdf(plot, PdfPageFormat.a4, fit: true));
    expect(fitted, greaterThan(3));
    paper.clear();
    expect(PlotSnapshot(paper).lines, isEmpty);
  });
}
