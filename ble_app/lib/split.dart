// Two panes with a draggable divider between them (2026-10-20): the wide
// layout's panes are resizable.
import 'package:flutter/material.dart';

class SplitPane extends StatefulWidget {
  const SplitPane({
    super.key,
    required this.first,
    required this.second,
    this.vertical = false,
    this.initial = 0.5,
    this.minSize = 80,
  });

  final Widget first, second;

  /// Stacked (divider across) rather than side by side.
  final bool vertical;

  /// The first pane's share of the space, to start with.
  final double initial;

  /// Neither pane is dragged smaller than this, in pixels.
  final double minSize;

  @override
  State<SplitPane> createState() => _SplitPaneState();
}

class _SplitPaneState extends State<SplitPane> {
  static const _grip = 8.0;
  late double _fraction = widget.initial;

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(builder: (context, box) {
      final total = (widget.vertical ? box.maxHeight : box.maxWidth) - _grip;
      final min = total < 2 * widget.minSize ? total / 2 : widget.minSize;
      final first = (total * _fraction).clamp(min, total - min);
      final line = Theme.of(context).dividerColor;
      final grip = MouseRegion(
        cursor: widget.vertical ? SystemMouseCursors.resizeRow : SystemMouseCursors.resizeColumn,
        child: GestureDetector(
          behavior: HitTestBehavior.opaque,
          onPanUpdate: (d) => setState(() {
            final pos = first + (widget.vertical ? d.delta.dy : d.delta.dx);
            _fraction = pos.clamp(min, total - min) / total;
          }),
          child: SizedBox(
            width: widget.vertical ? null : _grip,
            height: widget.vertical ? _grip : null,
            child: Center(
              child: Container(
                width: widget.vertical ? 32 : 2,
                height: widget.vertical ? 2 : 32,
                color: line,
              ),
            ),
          ),
        ),
      );
      final children = [
        SizedBox(width: widget.vertical ? null : first, height: widget.vertical ? first : null, child: widget.first),
        grip,
        Expanded(child: widget.second),
      ];
      return widget.vertical
          ? Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: children)
          : Row(crossAxisAlignment: CrossAxisAlignment.stretch, children: children);
    });
  }
}
