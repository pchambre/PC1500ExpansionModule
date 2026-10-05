// The PC-1500 Link protocol, server side: RP2350/BLE_PROTOCOL.md. Plain
// Dart with no Bluetooth in it, so it can be tested on its own: frames come
// in through handle() and go out through `send`.
//
// Every link is authenticated (sec.7, 2026-10-03): a PC-1500 pairs once
// with BLPAIR, this side's user checking the code it shows; after that its
// HELLOs prove it, and every frame is sealed. Until then only HELLO, BYE,
// AUTH and the pairing frames are answered -- files and the console are
// out of reach of anything not paired.
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'secure.dart';

/// Frame types (BLE_PROTOCOL.md sec.5).
class T {
  static const hello = 0x01, bye = 0x02, text = 0x10;
  static const auth = 0x03, pairStart = 0x04, pairNonce = 0x05, pairConfirm = 0x06; // sec.7
  static const filePut = 0x20, fileData = 0x21, fileEnd = 0x22, fileGet = 0x23, fileAbort = 0x24;
  static const plot = 0x40; // the CE-150 stand-in's drawing (2026-09-30)
  static const ack = 0x7E, err = 0x7F;
}

/// ERR codes.
class E {
  static const badFrame = 1, unsupported = 2, notFound = 3, exists = 4, io = 5, busy = 6, aborted = 7;
  static const notPaired = 8, authFailed = 9;
}

const protocolVersion = 2; // 2: sec.7's security
const kindServer = 2;
const targetServer = 0;
const kindUnknown = 0xFF;
const sizeUnknown = 0xFFFFFFFF;
const answerTimeout = Duration(seconds: 5);

class LinkServer {
  LinkServer({
    required this.send,
    required this.filesDir,
    required this.onText,
    required this.onLog,
    required this.pairings,
    this.onPlot,
    this.onPairRequest,
    this.name = 'PC1500-SRV',
  });

  /// This side's identity and the PC-1500s paired with it.
  final PairingStore pairings;

  /// A PC-1500's BLPAIR waiting for this side's user: the code to compare
  /// with the one it shows, then answerPairing(). Called again with null
  /// once the question is moot (answered, or the link gone).
  final void Function(String? code, String? peer)? onPairRequest;

  /// A PLOT payload (plot.dart's PlotPaper.add); false if malformed. Without
  /// one, PLOT is ERR UNSUPPORTED, which the PC-1500 reports as ERROR 27.
  final bool Function(Uint8List payload)? onPlot;

  /// One frame out (a TX notification).
  final Future<void> Function(Uint8List frame) send;
  final Directory filesDir;

  /// TEXT on the console channel, with CR turned into a newline; a form
  /// feed (BLCLS) is passed on, and means "clear the console".
  final void Function(String text) onText;
  final void Function(String line) onLog;
  final String name;

  /// The largest frame, MTU - 3 (sec.2); set when the MTU is known.
  int frameMax = 20;
  String? peerName;

  /// The link has authenticated: everything is sealed and allowed.
  bool get authenticated => _session != null;

  // sec.7: the HELLO's values, the session, a pairing under way
  Session? _session;
  Uint8List? _peerId, _nonceC, _nonceS, _ltk;
  int _pairStep = 0; // 1: keys exchanged, 2: nonces too, waiting on the user
  bool? _pairAnswer;
  PairKeys? _pairKeys;
  Uint8List? _pkC, _nS, _pairLtk;
  String? _pairName;

  int _txSeq = 0;
  int? _pendingSeq;
  Completer<int>? _pending; // 0 for ACK, else the ERR code
  Future<void> _queue = Future.value();

  // A save in progress.
  String? _putName;
  int _putSize = 0;
  BytesBuilder? _putData;

  /// Starts afresh for a new connection.
  void reset() {
    _txSeq = 0;
    _pending = null;
    _putName = null;
    _putData = null;
    peerName = null;
    _resetSecurity();
  }

  void _resetSecurity() {
    _session = null;
    _peerId = _nonceC = _nonceS = _ltk = null;
    _endPairing();
  }

  void _endPairing() {
    final asked = _pairStep == 2 && _pairAnswer == null;
    _pairStep = 0;
    _pairAnswer = null;
    _pairKeys = null;
    _pkC = _nS = _pairLtk = null;
    if (asked) onPairRequest?.call(null, null);
  }

  /// This side's user's answer to a PC-1500's pairing (onPairRequest).
  void answerPairing(bool accept) {
    if (_pairStep != 2 || _pairAnswer != null) return;
    _pairAnswer = accept;
    onPairRequest?.call(null, null);
    onLog(accept ? 'Pairing accepted; waiting for $_pairName' : 'Pairing refused');
  }

  /// One frame in. Answers (ACK/ERR) are taken at once -- a transfer this
  /// side is sending waits on them -- and everything else in order.
  void handle(Uint8List frame) {
    if (frame.length < 4 || frame.length - 4 != (frame[2] | frame[3] << 8)) {
      onLog('Bad frame (${frame.length} bytes)');
      return;
    }
    if (_session != null) {
      final plain = _session!.open(frame);
      if (plain == null) return onLog('Dropped a frame that failed authentication');
      frame = plain;
    }
    final type = frame[0], seq = frame[1];
    if (type == T.ack || type == T.err) {
      if (_pending != null && seq == _pendingSeq && !_pending!.isCompleted) {
        _pending!.complete(type == T.ack ? 0 : (frame.length > 4 ? frame[4] : E.badFrame));
      }
      return;
    }
    final payload = Uint8List.sublistView(frame, 4);
    _queue = _queue.then((_) => _process(type, seq, payload)).catchError((Object e) => onLog('Error: $e'));
  }

  Future<void> _process(int type, int seq, Uint8List p) async {
    const open = {T.hello, T.bye, T.auth, T.pairStart, T.pairNonce, T.pairConfirm};
    if (_session == null && !open.contains(type)) return _answer(seq, E.notPaired);
    switch (type) {
      case T.hello:
        await _hello(seq, p);
      case T.auth:
        await _auth(seq, p);
      case T.pairStart:
      case T.pairNonce:
      case T.pairConfirm:
        await _pair(type, seq, p);
      case T.bye:
        onLog('BYE');
        await _answer(seq);
      case T.text:
        if (p.isEmpty) return _answer(seq, E.badFrame);
        if (p[0] == 0) onText(latin1.decode(p.sublist(1), allowInvalid: true).replaceAll('\r', '\n'));
        await _answer(seq);
      case T.filePut:
        await _filePut(seq, p);
      case T.fileData:
        if (_putData == null) return _answer(seq, E.badFrame);
        _putData!.add(p);
        await _answer(seq);
      case T.fileEnd:
        await _fileEnd(seq);
      case T.fileAbort:
        if (_putName != null) onLog('Save of $_putName abandoned');
        _putName = null;
        _putData = null;
        await _answer(seq);
      case T.fileGet:
        await _fileGet(seq, p);
      case T.plot:
        if (onPlot == null) return _answer(seq, E.unsupported);
        await _answer(seq, onPlot!(Uint8List.fromList(p)) ? 0 : E.badFrame);
      default:
        await _answer(seq, E.unsupported);
    }
  }

  // ---- sec.7 ----

  // version, kind, name (str8), id 8, nonce 16. Ours adds `known` and, if
  // known, our proof.
  Future<void> _hello(int seq, Uint8List p) async {
    final n = p.length >= 3 ? p[2] : 0;
    if (p.length < 3 + n + idLen + nonceLen || p[0] != protocolVersion) {
      onLog('A PC-1500 with older firmware was refused: update it for pairing');
      return _answer(seq, E.unsupported);
    }
    _resetSecurity();
    peerName = latin1.decode(p.sublist(3, 3 + n));
    _peerId = Uint8List.fromList(p.sublist(3 + n, 3 + n + idLen));
    _nonceC = Uint8List.fromList(p.sublist(3 + n + idLen, 3 + n + idLen + nonceLen));
    _nonceS = randomBytes(nonceLen);
    final pairing = pairings.find(_peerId!);
    _ltk = pairing?.ltk;
    onLog('HELLO from $peerName${pairing == null ? " (not paired)" : ""}');
    await _answer(seq);
    final nameBytes = latin1.encode(name);
    final r = await _request(T.hello, [
      protocolVersion, kindServer, nameBytes.length, ...nameBytes, //
      ...pairings.id, ..._nonceS!, pairing == null ? 0 : 1,
      if (pairing != null) ...authProof(pairing.ltk, 'S', _nonceC!, _nonceS!, _peerId!, pairings.id),
    ]);
    if (r != 0) onLog('Our HELLO was refused ($r)');
  }

  // The PC-1500's proof. The session starts before the ACK -- the last
  // frame in the clear -- goes out, so nothing it seals next is missed.
  Future<void> _auth(int seq, Uint8List p) async {
    final ltk = _ltk;
    if (ltk == null) return _answer(seq, E.notPaired);
    if (_session != null || p.length != proofLen) return _answer(seq, E.badFrame);
    if (!equal16(p, authProof(ltk, 'C', _nonceC!, _nonceS!, _peerId!, pairings.id))) {
      onLog('$peerName failed to prove itself');
      return _answer(seq, E.authFailed);
    }
    _session = Session.start(ltk, _nonceC!, _nonceS!, connector: false);
    _ltk = null;
    onLog('$peerName authenticated');
    await send(_frame(T.ack, seq, const []));
  }

  // PAIR_START [pk_c] -> [pk_s][commit]; PAIR_NONCE [n_c] -> [n_s], and the
  // code for the user; PAIR_CONFIRM [ok][mac_c] -> BUSY until the user
  // answers, then [0] (refused) or [1][mac_s] (paired).
  Future<void> _pair(int type, int seq, Uint8List p) async {
    if (_session != null) return _answer(seq, E.badFrame);
    switch (type) {
      case T.pairStart when p.length == pubLen:
        _endPairing();
        _pairKeys = await PairKeys.fromRandom(randomBytes(32));
        _pkC = Uint8List.fromList(p);
        _nS = randomBytes(nonceLen);
        _pairStep = 1;
        return _answer(seq, 0, [..._pairKeys!.publicKey, ...pairCommit(_pairKeys!.publicKey, _pkC!, _nS!)]);
      case T.pairNonce when p.length == nonceLen && _pairStep == 1:
        final pkS = _pairKeys!.publicKey;
        _pairLtk = _pairKeys!.ltk(_pkC!, _pkC!, pkS, p, _nS!);
        _pairKeys = null;
        if (_pairLtk == null) {
          _endPairing();
          return _answer(seq, E.authFailed);
        }
        _pairStep = 2;
        _pairName = peerName ?? 'PC-1500';
        final code = codeText(pairCode(_pkC!, pkS, p, _nS!));
        onLog('$_pairName wants to pair: code $code');
        await _answer(seq, 0, _nS!);
        onPairRequest?.call(code, _pairName);
      case T.pairConfirm when p.length == 1 + proofLen && _pairStep == 2:
        if (_pairAnswer == null) return _answer(seq, E.busy);
        if (_pairAnswer == false || p[0] != 1) {
          _endPairing();
          return _answer(seq, 0, const [0]);
        }
        final ltk = _pairLtk!;
        if (!equal16(Uint8List.sublistView(p, 1), pairConfirm(ltk, 'C', _peerId!, pairings.id))) {
          _endPairing();
          onLog('Pairing failed: the codes were not the same');
          return _answer(seq, E.authFailed);
        }
        await pairings.add(Pairing(Uint8List.fromList(_peerId!), _pairName!, ltk));
        onLog('Paired with $_pairName');
        final mac = pairConfirm(ltk, 'S', _peerId!, pairings.id);
        _endPairing();
        await _answer(seq, 0, [1, ...mac]);
      default:
        _endPairing();
        await _answer(seq, E.badFrame);
    }
  }

  // target, kind, flags, size (u32 LE), name (str8)
  Future<void> _filePut(int seq, Uint8List p) async {
    if (p.length < 8 || 8 + p[7] > p.length) return _answer(seq, E.badFrame);
    if (p[0] != targetServer) return _answer(seq, E.unsupported);
    if (_putData != null) return _answer(seq, E.busy);
    final name = _fileName(p.sublist(8, 8 + p[7]));
    if (name == null) return _answer(seq, E.io);
    final overwrite = p[2] & 1 != 0;
    if (!overwrite && File(_path(name)).existsSync()) return _answer(seq, E.exists);
    _putName = name;
    _putSize = ByteData.sublistView(p, 3, 7).getUint32(0, Endian.little);
    _putData = BytesBuilder();
    onLog('Saving $name');
    await _answer(seq);
  }

  Future<void> _fileEnd(int seq) async {
    final data = _putData, name = _putName;
    _putData = null;
    _putName = null;
    if (data == null || name == null) return _answer(seq, E.badFrame);
    final bytes = data.takeBytes();
    if (_putSize != sizeUnknown && bytes.length != _putSize) {
      onLog('$name: got ${bytes.length} bytes, expected $_putSize');
      return _answer(seq, E.io);
    }
    try {
      filesDir.createSync(recursive: true);
      File(_path(name)).writeAsBytesSync(bytes);
    } catch (e) {
      onLog('$name: $e');
      return _answer(seq, E.io);
    }
    onLog('Saved $name (${bytes.length} bytes)');
    await _answer(seq);
  }

  // target, name (str8)
  Future<void> _fileGet(int seq, Uint8List p) async {
    if (p.length < 2 || 2 + p[1] > p.length) return _answer(seq, E.badFrame);
    if (p[0] != targetServer) return _answer(seq, E.unsupported);
    final name = _fileName(p.sublist(2, 2 + p[1]));
    final file = name == null ? null : File(_path(name));
    if (file == null || !file.existsSync()) return _answer(seq, E.notFound);
    final bytes = file.readAsBytesSync();
    await _answer(seq);
    onLog('Sending $name (${bytes.length} bytes)');
    final nameBytes = latin1.encode(name!);
    final header = BytesBuilder()
      ..add([targetServer, kindUnknown, 0])
      ..add((ByteData(4)..setUint32(0, bytes.length, Endian.little)).buffer.asUint8List())
      ..add([nameBytes.length, ...nameBytes]);
    if (await _request(T.filePut, header.takeBytes()) != 0) return onLog('$name: refused');
    final chunk = frameMax - 4 - sealOverhead;
    for (var at = 0; at < bytes.length; at += chunk) {
      final end = at + chunk < bytes.length ? at + chunk : bytes.length;
      final r = await _request(T.fileData, bytes.sublist(at, end));
      if (r != 0) return onLog('$name: stopped at byte $at ($r)');
    }
    if (await _request(T.fileEnd, const []) == 0) onLog('Sent $name');
  }

  /// A plain file name for the store: no folders, nothing Windows rejects.
  String? _fileName(List<int> raw) {
    final name = latin1.decode(raw).trim();
    if (name.isEmpty || name == '.' || name == '..' || RegExp(r'[\\/:*?"<>|\x00-\x1F]').hasMatch(name)) return null;
    return name;
  }

  String _path(String name) => '${filesDir.path}${Platform.pathSeparator}$name';

  /// ACK (with a pairing answer's payload) or ERR.
  Future<void> _answer(int seq, [int error = 0, List<int> payload = const []]) =>
      _send(_frame(error == 0 ? T.ack : T.err, seq, error == 0 ? payload : [error]));

  /// Sealed once the link has authenticated.
  Future<void> _send(Uint8List frame) {
    final s = _session;
    if (s == null) return send(frame);
    final sealed = s.seal(frame);
    return sealed == null ? Future.value() : send(sealed);
  }

  /// A frame out and its answer: 0 for ACK, else the ERR code (or
  /// E.aborted if none came -- sec.4).
  Future<int> _request(int type, List<int> payload) async {
    final seq = _txSeq;
    _txSeq = (_txSeq + 1) & 0xFF;
    _pendingSeq = seq;
    _pending = Completer<int>();
    await _send(_frame(type, seq, payload));
    return _pending!.future.timeout(answerTimeout, onTimeout: () {
      onLog('No answer from the PC-1500');
      return E.aborted;
    });
  }

  static Uint8List _frame(int type, int seq, List<int> payload) =>
      Uint8List.fromList([type, seq, payload.length & 0xFF, payload.length >> 8, ...payload]);
}
