// Copyright (c) 2015, the Fletch project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

library servicec.plugins.java;

import 'dart:core' hide Type;

import 'package:path/path.dart' show basenameWithoutExtension, join;
import 'package:strings/strings.dart' as strings;

import '../emitter.dart';
import '../parser.dart';

import 'cc.dart' show CcVisitor;

const HEADER = """
// Copyright (c) 2015, the Fletch project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE.md file.

// Generated file. Do not edit.
""";

const FLETCH_API_JAVA = """
package fletch;

public class FletchApi {
  public static native void Setup();
  public static native void TearDown();
  public static native void RunSnapshot(byte[] snapshot);
  public static native void AddDefaultSharedLibrary(String library);
}
""";

const FLETCH_SERVICE_API_JAVA = """
package fletch;

public class FletchServiceApi {
  public static native void Setup();
  public static native void TearDown();
}
""";

const FLETCH_API_JAVA_IMPL = """
#include <jni.h>

#include "fletch_api.h"

#ifdef __cplusplus
extern "C" {
#endif

JNIEXPORT void JNICALL Java_fletch_FletchApi_Setup(JNIEnv*, jclass) {
  FletchSetup();
}

JNIEXPORT void JNICALL Java_fletch_FletchApi_TearDown(JNIEnv*, jclass) {
  FletchTearDown();
}

JNIEXPORT void JNICALL Java_fletch_FletchApi_RunSnapshot(JNIEnv* env,
                                                         jclass,
                                                         jbyteArray snapshot) {
  int len = env->GetArrayLength(snapshot);
  unsigned char* copy = new unsigned char[len];
  env->GetByteArrayRegion(snapshot, 0, len, reinterpret_cast<jbyte*>(copy));
  FletchRunSnapshot(copy, len);
  delete copy;
}

JNIEXPORT void JNICALL Java_fletch_FletchApi_AddDefaultSharedLibrary(
    JNIEnv* env, jclass, jstring str) {
  const char* library = env->GetStringUTFChars(str, 0);
  FletchAddDefaultSharedLibrary(library);
  env->ReleaseStringUTFChars(str, library);
}

#ifdef __cplusplus
}
#endif
""";

const FLETCH_SERVICE_API_JAVA_IMPL = """
#include <jni.h>

#include "service_api.h"

#ifdef __cplusplus
extern "C" {
#endif

JNIEXPORT void JNICALL Java_fletch_FletchServiceApi_Setup(JNIEnv*, jclass) {
  ServiceApiSetup();
}

JNIEXPORT void JNICALL Java_fletch_FletchServiceApi_TearDown(JNIEnv*, jclass) {
  ServiceApiTearDown();
}

#ifdef __cplusplus
}
#endif
""";

const JNI_ATTACH_DETACH = """
static JNIEnv* attachCurrentThreadAndGetEnv(JavaVM* vm) {
  void* result = NULL;
  if (vm->AttachCurrentThread(&result, NULL) != JNI_OK) {
    // TODO(ager): Nicer error recovery?
    printf("Failed to attach callback thread to Java VM.");
    exit(1);
  }
  return reinterpret_cast<JNIEnv*>(result);
}

static void detachCurrentThread(JavaVM* vm) {
  if (vm->DetachCurrentThread() != JNI_OK) {
    // TODO(ager): Nicer error recovery?
    printf("Failed to detach callback thread from Java VM.");
    exit(1);
  }
}""";

void generate(String path, Unit unit, String outputDirectory) {
  _generateFletchApis(outputDirectory);
  _generateServiceJava(path, unit, outputDirectory);
  _generateServiceJni(path, unit, outputDirectory);
}

void _generateFletchApis(String outputDirectory) {
  String fletchDirectory = join(outputDirectory, 'java', 'fletch');
  String jniDirectory = join(outputDirectory, 'java', 'jni');

  StringBuffer buffer = new StringBuffer(HEADER);
  buffer.writeln();
  buffer.write(FLETCH_API_JAVA);
  writeToFile(fletchDirectory, 'FletchApi', 'java', buffer.toString());

  buffer = new StringBuffer(HEADER);
  buffer.writeln();
  buffer.write(FLETCH_SERVICE_API_JAVA);
  writeToFile(fletchDirectory, 'FletchServiceApi', 'java', buffer.toString());

  buffer = new StringBuffer(HEADER);
  buffer.writeln();
  buffer.write(FLETCH_API_JAVA_IMPL);
  writeToFile(jniDirectory, 'fletch_api_wrapper', 'cc', buffer.toString());

  buffer = new StringBuffer(HEADER);
  buffer.writeln();
  buffer.write(FLETCH_SERVICE_API_JAVA_IMPL);
  writeToFile(jniDirectory,
              'fletch_service_api_wrapper',
              'cc',
              buffer.toString());
}

void _generateServiceJava(String path, Unit unit, String outputDirectory) {
  _JavaVisitor visitor = new _JavaVisitor(path);
  visitor.visit(unit);
  String contents = visitor.buffer.toString();
  String directory = join(outputDirectory, 'java', 'fletch');
  // TODO(ager): We should generate a file per service here.
  if (unit.services.length > 1) {
    print('Java plugin: multiple services in one file is not supported.');
  }
  String serviceName = unit.services.first.name;
  writeToFile(directory, serviceName, "java", contents);
}

void _generateServiceJni(String path, Unit unit, String outputDirectory) {
  _JniVisitor visitor = new _JniVisitor(path);
  visitor.visit(unit);
  String contents = visitor.buffer.toString();
  String directory = join(outputDirectory, 'java', 'jni');
  // TODO(ager): We should generate a file per service here.
  if (unit.services.length > 1) {
    print('Java plugin: multiple services in one file is not supported.');
  }
  String serviceName = unit.services.first.name;
  String file = '${strings.underscore(serviceName)}_wrapper';
  writeToFile(directory, file, "cc", contents);
}

class _JavaVisitor extends Visitor {
  final String path;
  final StringBuffer buffer = new StringBuffer();

  _JavaVisitor(this.path);

  visit(Node node) => node.accept(this);

  visitUnit(Unit node) {
    buffer.writeln(HEADER);
    buffer.writeln('package fletch;');
    node.services.forEach(visit);
  }

  visitService(Service node) {
    buffer.writeln();
    buffer.writeln('public class ${node.name} {');
    buffer.writeln('  public static native void Setup();');
    buffer.writeln('  public static native void TearDown();');
    node.methods.forEach(visit);
    buffer.writeln('}');
  }

  visitMethod(Method node) {
    String name = node.name;
    buffer.writeln();
    buffer.writeln('  public static abstract class ${name}Callback {');
    buffer.write('    public abstract void handle(');
    visit(node.returnType);
    buffer.writeln(' result);');
    buffer.writeln('  }');

    buffer.writeln();
    buffer.write('  public static native ');
    visit(node.returnType);
    buffer.write(' ${name}(');
    visitArguments(node.arguments);
    buffer.writeln(');');
    buffer.write('  public static native void ${name}Async(');
    visitArguments(node.arguments);
    if (!node.arguments.isEmpty) buffer.write(', ');
    buffer.writeln('${name}Callback callback);');
  }

  visitArguments(List<Formal> formals) {
    bool first = true;
    formals.forEach((Formal formal) {
      if (!first) buffer.write(', ');
      first = false;
      visit(formal);
    });
  }

  visitFormal(Formal node) {
    visit(node.type);
    buffer.write(' ${node.name}');
  }

  visitType(Type node) {
    Map<String, String> types = const { 'Int32': 'int' };
    String type = types[node.identifier];
    buffer.write(type);
  }
}

class _JniVisitor extends CcVisitor {
  int methodId = 1;
  String serviceName;

  _JniVisitor(String path) : super(path);

  visit(Node node) => node.accept(this);

  visitUnit(Unit node) {
    buffer.writeln(HEADER);
    buffer.writeln('#include <jni.h>');
    buffer.writeln('#include <stdlib.h>');
    buffer.writeln();
    buffer.writeln('#include "service_api.h"');
    node.services.forEach(visit);
  }

  visitService(Service node) {
    serviceName = node.name;

    buffer.writeln();

    buffer.writeln('#ifdef __cplusplus');
    buffer.writeln('extern "C" {');
    buffer.writeln('#endif');

    buffer.writeln();
    buffer.writeln('static ServiceId _service_id = kNoServiceId;');

    buffer.writeln();
    buffer.write('JNIEXPORT void JNICALL Java_fletch_');
    buffer.writeln('${serviceName}_Setup(JNIEnv*, jclass) {');
    buffer.writeln('  _service_id = ServiceApiLookup("$serviceName");');
    buffer.writeln('}');

    buffer.writeln();
    buffer.write('JNIEXPORT void JNICALL Java_fletch_');
    buffer.writeln('${serviceName}_TearDown(JNIEnv*, jclass) {');
    buffer.writeln('  ServiceApiTerminate(_service_id);');
    buffer.writeln('}');

    buffer.writeln();
    buffer.writeln(JNI_ATTACH_DETACH);

    node.methods.forEach(visit);

    buffer.writeln();
    buffer.writeln('#ifdef __cplusplus');
    buffer.writeln('}');
    buffer.writeln('#endif');
  }

  visitMethod(Method node) {
    String name = node.name;
    String id = '_k${name}Id';

    buffer.writeln();
    buffer.write('static const MethodId $id = ');
    buffer.writeln('reinterpret_cast<MethodId>(${methodId++});');

    buffer.writeln();
    buffer.write('JNIEXPORT ');
    visit(node.returnType);
    buffer.write(' JNICALL Java_fletch_${serviceName}_${name}(');
    buffer.write('JNIEnv*, jclass, ');
    visitArguments(node.arguments);
    buffer.writeln(') {');
    visitMethodBody(id, node.arguments);
    buffer.writeln('}');

    String callback = ensureCallback(node.returnType, node.arguments);

    buffer.writeln();
    buffer.write('JNIEXPORT void JNICALL ');
    buffer.write('Java_fletch_${serviceName}_${name}Async(');
    buffer.write('JNIEnv* _env, jclass, ');
    visitArguments(node.arguments);
    buffer.writeln(', jobject _callback) {');
    buffer.writeln('  jobject callback = _env->NewGlobalRef(_callback);');
    buffer.writeln('  JavaVM* vm;');
    buffer.writeln('  _env->GetJavaVM(&vm);');
    visitMethodBody(id,
                    node.arguments,
                    extraArguments: [ 'vm' ],
                    callback: callback);
    buffer.writeln('}');
  }

  visitType(Type node) {
    Map<String, String> types = const { 'Int32': 'jint' };
    String type = types[node.identifier];
    buffer.write(type);
  }

  final Map<String, String> callbacks = {};
  String ensureCallback(Type type, List<Formal> arguments,
                        {bool cStyle: false}) {
    String key = '${type.identifier}_${arguments.length}';
    return callbacks.putIfAbsent(key, () {
      String cast(String type) => CcVisitor.cast(type, cStyle);
      String name = 'Unwrap_$key';
      buffer.writeln();
      buffer.writeln('static void $name(void* raw) {');
      buffer.writeln('  char* buffer = ${cast('char*')}(raw);');
      buffer.writeln('  int result = *${cast('int*')}(buffer + 32);');
      int offset = 32 + (arguments.length * 4);
      buffer.write('  jobject callback = *${cast('jobject*')}');
      buffer.writeln('(buffer + $offset);');
      buffer.write('  JavaVM* vm = *${cast('JavaVM**')}');
      buffer.writeln('(buffer + $offset + sizeof(void*));');
      buffer.writeln('  JNIEnv* env = attachCurrentThreadAndGetEnv(vm);');
      buffer.writeln('  jclass clazz = env->GetObjectClass(callback);');
      buffer.write('  jmethodID methodId = env->GetMethodID');
      // TODO(ager): For now the return type is hard-coded to int.
      buffer.writeln('(clazz, "handle", "(I)V");');
      buffer.writeln('  env->CallVoidMethod(callback, methodId, result);');
      buffer.writeln('  env->DeleteGlobalRef(callback);');
      buffer.writeln('  detachCurrentThread(vm);');
      buffer.writeln('  free(buffer);');
      buffer.writeln('}');
      return name;
    });
  }
}