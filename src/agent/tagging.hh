/**
  * Functions for tagging objects.
  *
  * All functions in this module require the global lock to be taken.
  */
#ifndef TAGGING_HH
#define TAGGING_HH

#include <jvmti.h>
#include <jni.h>
#include <map>
#include <stack>
#include <vector>
#include "NativeInterface.h"
#include <sstream>
#include <set>
#include "callstack.hh"
#include <sys/types.h>
#include <sys/dir.h>

std::atomic_long nextObjID(NativeInterface_SPECIAL_VAL_MAX+1);
//arst std::map<long, std::vector<std::pair<long, std::string> > > stacks;
std::map<std::string, callstack> stacks;
std::map<std::string, std::stack<long> > freshObjectIDs;
std::set<std::string> instrumentedClasses;
std::set<std::string> uninstrumentedClasses;

void ensureThreadKnown(const std::string &threadName) {
  if (stacks.find(threadName) == stacks.end()) {
    DBG("making thread #" << threadName << " known");
    // accessing, as a side effect, creates the object:
    stacks[threadName];
    freshObjectIDs[threadName];
  }
  ASSERT(freshObjectIDs.find(threadName) != freshObjectIDs.end());
}

callstack &getThreadStack(std::string threadName) {
  DBG("getting stack for thread '"<<threadName<<"'");
  if (stacks.find(threadName) != stacks.end()) {
    ensureThreadKnown(threadName);
  }
  return stacks[threadName];
}

void printCurrentStack(std::string threadName) {
  DBG("stack of thread " << threadName << " is now:");
  DBG("------------");
  const auto & threadStack = getThreadStack(threadName);
  for (int i=0; i<threadStack.hasFrames(); ++i) {
     DBG(" ******************  ["
     <<threadStack.peekFrame(i).getThisId()<<"@"
     <<threadStack.peekFrame(i).getClassName() << "::"
     <<threadStack.peekFrame(i).getMethodName() << "]");
  }
  //DBG("... omitted ..");
  DBG("------------");
}

bool fileExists(const std::string &path) {
  FILE *file;
  if ((file = fopen(path.c_str(), "r")) != NULL) {
    fclose(file);
    return true;
  } else {
    return false;
  }
}

bool isClassInstrumented(const std::string &className) {
  return (
    className.c_str()[0] == '['
      ||
    instrumentedClasses.find(className) != instrumentedClasses.end()
  );
}

void markClassAsInstrumented(const std::string &className) {
  DBG("marking "<<className<<" as instrumented");
  instrumentedClasses.insert(className);
  ASSERT(isClassInstrumented(className));
}

void markClassFilesAsInstrumented(const std::string &directory, const std::string &package = "") {
  DIR *dir;
  struct dirent *ent;
  if ((dir = opendir ((directory+"/"+package).c_str())) != NULL) {

    while ((ent = readdir (dir)) != NULL) {
      if (ent->d_name[0] != '.') {
        std::string fileName = package+"/"+ent->d_name;
        size_t appendixSize = std::string(".class").size();
        if (fileName.size() >= appendixSize) {
          auto stem = fileName.substr(1,fileName.size()-appendixSize-1);
          auto extension = fileName.substr(fileName.size()-appendixSize, appendixSize);
          if (extension == ".class") {
            DBG("adding "<<stem<<" to instrumented class files");
            markClassAsInstrumented(stem);
            continue;
          }
        }
        markClassFilesAsInstrumented(directory, package+"/"+ent->d_name);
      }
    }
    closedir (dir);
  }
}

void markClassAsUninstrumented(const std::string &className) {
  DBG("marking class "<<className<<" as uninstrumented");
  ASSERT(!isClassInstrumented(className));
  uninstrumentedClasses.insert(className);
  ASSERT(!isClassInstrumented(className));
}

//void handleValStatic(JNIEnv *env, jint *objectkind, jobject *jobj,
//                     jstring klassname);

std::string toStdString(JNIEnv *env, jstring str);

long doTag(jvmtiEnv *jvmti, jobject jobj);

long getOrDoTag(jint objkind, jobject jobj, std::string klass, const std::string &thread,
                jvmtiEnv *g_jvmti, jrawMonitorID g_lock);

std::string getThreadName(jvmtiEnv* g_jvmti, jrawMonitorID g_lock) {
  jthread thread;
  jvmtiError err = g_jvmti->GetCurrentThread(&thread);

  ASSERT_NO_JVMTI_ERR(g_jvmti, err);

  std::string threadName;

  jvmtiThreadInfo info;
  err = g_jvmti->GetThreadInfo(thread, &info);
  if (err == JVMTI_ERROR_WRONG_PHASE) {
    if (thread == NULL) {
      threadName = "JVM_Thread<0x0>";
    } else {
      DBG("tagging startup thread "<<thread);
      // GetThreadInfo works only during live phase
      std::stringstream ss;
      jlong tag;
      err = g_jvmti->GetTag(thread, &tag);
      ASSERT_NO_JVMTI_ERR(g_jvmti, err);
      if (tag == 0) {
        tag = nextObjID.fetch_add(20);
        err = g_jvmti->SetTag(thread, tag);
        ASSERT_NO_JVMTI_ERR(g_jvmti, err);
      }
        //getOrDoTag(NativeInterface_SPECIAL_VAL_NORMAL, thread, "java/lang/Thread", "wat", *g_jvmti, g_lock);
      //err = g_jvmti->SetTag(thread, tag);

      ss<<"JVM_Thread<"<<tag<<">";
      threadName = ss.str();
    }
  } else {
    threadName = info.name;
    ASSERT_NO_JVMTI_ERR(g_jvmti, err);
  }
  ensureThreadKnown(threadName);
  DBG("done getting thread name");
  return threadName;
}

void pushStackFrame(JNIEnv *env, const std::string &thread, jint callee_kind,
                    jobject callee, jstring mname, jstring cname,
                    jvmtiEnv *g_jvmti, jrawMonitorID g_lock) {
  DBG("pushing stack frame");

  std::string threadName = getThreadName(g_jvmti, g_lock);

  jlong calleeTag;
  if (callee_kind == NativeInterface_SPECIAL_VAL_THIS) {
    // create a new object ID and push it to the stack.
    // handleValStatic will then use this ID whenever it sees a
    // SPECIAL_VAL_THIS!

    ASSERT_MSG(!freshObjectIDs[threadName].empty(),
               "have no object ID for SPECIAL_VAL_THIS (method "<<toStdString(env, mname)
               <<", class "<<toStdString(env,cname)<<")");
    calleeTag = freshObjectIDs[threadName].top();
    DBG("thread " << threadName << ": generating tag " << calleeTag
                  << " for SPECIAL_VAL_THIS::"<<toStdString(env,cname));
    //freshObjectIDs[threadName].push(calleeTag);
  } else {
    //handleValStatic(env, &callee_kind, &callee, cname);
    calleeTag = getOrDoTag(callee_kind, callee, toStdString(env, cname),
                            threadName, g_jvmti, g_lock);
  }
  std::string mnameStr = toStdString(env, mname);
  std::string cnameStr = toStdString(env, cname);
  ASSERT(mnameStr != "java/security/BasicPermission");
  getThreadStack(threadName)
      .pushFrame(mnameStr, cnameStr, calleeTag);
  ASSERT(getThreadStack(threadName).hasFrames() > 0);
  ASSERT(getThreadStack(threadName).peekFrame().getMethodName() != "");
  DBG("pushing stack frame done");
  printCurrentStack(threadName);
}

long getTag(jint objkind, jobject jobj, std::string klass, const std::string &threadName,
            jvmtiEnv *g_jvmti);

bool tagFreshlyInitialisedObject(jvmtiEnv *g_jvmti, jobject callee,
                                 std::string threadName) {
  DBG("tagFreshlyInitialisedObject(..., threadName = " << threadName << ")");
  ASSERT(freshObjectIDs.find(threadName) != freshObjectIDs.end() &&
         "need to know the thread");
  if (getTag(NativeInterface_SPECIAL_VAL_NORMAL, callee, "class/unknown", threadName, g_jvmti) != 0) {
    return false;
  }
  ASSERT(g_jvmti);
  ASSERT(callee);
  //doTag(g_jvmti, callee);

  jlong tag;
  //  if (freshObjectIDs[threadName].empty()) {
  DBG("thread " << threadName << ": don't have an object ID");
  tag = nextObjID.fetch_add(1);
  freshObjectIDs[threadName].push(tag);
  //} else {
  //  tag = freshObjectIDs[threadName].top();
  //  freshObjectIDs[threadName].pop();
  //}
  DBG("tagging freshly initialised object with id " << tag);
  jvmtiError err = g_jvmti->SetTag(callee, tag);
  ASSERT_NO_JVMTI_ERR(g_jvmti, err);
  return true;
  // DBG("set tag " << tag << " on " << jobj);
}

void popStackFrame(const std::string &threadName) {
  //if (getThreadStack(threadName).hasFrames() == 0) {
  //  WARN("trying to pop from empty stack model for thread" << threadName);
  //} else {
  if (getThreadStack(threadName).hasFrames() <= 0) {
    ERR("popping frame from empty stack");
    return;
  }
  const auto &oldTop = getThreadStack(threadName).peekFrame();
  if (oldTop.getClassName() != "JvmInternals" && oldTop.getMethodName() != "jvmInternals") {
    getThreadStack(threadName).popFrame();
    DBG("popped frame ["<<oldTop.getThisId()<<"@"<<oldTop.getClassName()<<"::"<<oldTop.getMethodName()<<"]");
  } else {
    ERR("did not pop frame of magic method JvmInternals::jvmInternals");
  }
  //}
  printCurrentStack(threadName);
}

const frame getRunningFrame(const std::string &threadName) {
  if (getThreadStack(threadName).hasFrames() <= 0) {
    ERR("getting from from empty stack");
    frame f("unregistered method", "unregistered class", NativeInterface_SPECIAL_VAL_NOT_IMPLEMENTED);
    return f;
  }
  return getThreadStack(threadName).peekFrame();
}

std::string getRunningFunction(const std::string &threadName) {
  auto ret = getRunningFrame(threadName);
  ASSERT(ret.getMethodName() != "");
  ASSERT(ret.getClassName() != "");
  return ret.getMethodName();
}

long getRunningObject(const std::string &threadName) { return getRunningFrame(threadName).getThisId(); }

const std::string getRunningClass(const std::string &threadName) { return getRunningFrame(threadName).getClassName(); }

/**
  Tag an object with a fresh ID.

  Requires that the object hasn't been tagged before (warns otherwise).

*/
long doTag(jvmtiEnv *g_jvmti, jobject jobj) {
  jlong tag;
  jvmtiError err;
  err = g_jvmti->GetTag(jobj, &tag);
  //ASSERT(tag == 0);
  tag = nextObjID.fetch_add(1);
  err = g_jvmti->SetTag(jobj, tag);
  //ASSERT_NO_JVMTI_ERR(g_jvmti, err);
  //ASSERT(tag != 0);
  // DBG("set tag " << tag << " on " << jobj);
  return tag;
}

#define NATIVE_CLASSREPS

#ifdef NATIVE_CLASSREPS
std::map<const std::string, const jlong> classReps;

jlong getClassRepTag(const std::string &className) {
  if (classReps.find(className) == classReps.end()) {
    jlong tag = -nextObjID.fetch_add(1);
    classReps.insert({ { className, tag } });
    return tag;
  } else {
    return classReps[className];
  }
}
#endif

long getTag(jint objkind, jobject jobj, std::string klass, const std::string &threadName,
            jvmtiEnv *g_jvmti) {
  jlong tag = 0;
    switch (objkind) {
  case NativeInterface_SPECIAL_VAL_NORMAL: {
    if (jobj) {
      jvmtiError err = g_jvmti->GetTag(jobj, &tag);
      // DBG("getting preliminary tag " << tag << " from " << jobj);
      DBG("getting tag (" << klass << " @ " << tag << ") from JVMTI");
      ASSERT_NO_JVMTI_ERR(g_jvmti, err);
      }
    }
    break;
   case NativeInterface_SPECIAL_VAL_THIS: {
     if (getThreadStack(threadName).hasFrames() > 0) {
       ASSERT(getThreadStack(threadName).hasFrames() > 0);
       tag = getThreadStack(threadName).peekFrame().getThisId();
       DBG("getting tag (" << klass << " @ " << tag << ") from top");
     } else {
       DBG("getting tag (" << klass << " @ " << NativeInterface_SPECIAL_VAL_JVM
                           << ") from SPECIAL_VAL_JVM");
       tag = NativeInterface_SPECIAL_VAL_JVM;
     }
     break;
  }
  case NativeInterface_SPECIAL_VAL_STATIC: {
    tag = getClassRepTag(klass);
    DBG("getting tag (" << klass << " @ " << tag << ") from classRep");
    break;
  }
  case NativeInterface_SPECIAL_VAL_NOT_IMPLEMENTED: {
    ERR("SPECIAL_VAL_NOT_IMPLEMENTED");
    break;
  }
  default:
    ERR("WAT? "<<objkind);
  }

  return tag;
}

long getOrDoTag(jint objkind, jobject jobj, std::string klass, const std::string &thread,
                jvmtiEnv *g_jvmti, jrawMonitorID g_lock) {
  // LOCK;

  jlong tag = getTag(objkind, jobj, klass, thread, g_jvmti);
  if (tag == 0) {
    tag = doTag(g_jvmti, jobj);
    DBG("setting tag (" << klass << " @ " << tag << ") using doTag");
    //ASSERT(getOrDoTag(objkind, jobj, klass, thread, g_jvmti, g_lock) ==
    //       tag);
    ASSERT(tag != 0);
  }
  return tag;
}

/*
  returns a c++ string with content copied from a java str
*/
std::string toStdString(JNIEnv *env, jstring str) {
  // BUG: if I comment stuff below back in: crashes!
  if (str == NULL) {
    return "NULL";
  }
  const char *c_str = env->GetStringUTFChars(str, NULL);
  const std::string result(c_str);
  env->ReleaseStringUTFChars(str, c_str);
  return result;
}

/**

If the object kind is SPECIAL_VAL_STATIC, replace the reference by a
reference to the class' representative.

*/

//void handleValStatic(JNIEnv *env, jint *objectkind, jobject *jobj,
//                     jlong *tag, jstring klassName) {
//   DBG("handleValStatic");
//   if (*objectkind == NativeInterface_SPECIAL_VAL_STATIC) {
//
//#ifdef NATIVE_CLASSREPS
//    *tag = getClassRepTag(klassName);
//#else
//    jclass cls = env->FindClass("NativeInterface");
//    jmethodID getClassRep =
//        env->GetStaticMethodID(cls, "getClassRepresentative",
//                               "(Ljava/lang/String;)Ljava/lang/Object;");
//    jclass nativeIF = (jclass)env->NewGlobalRef(cls);
//    *jobj = env->CallStaticObjectMethod(nativeIF, getClassRep, klassname);
//    *objectkind = NativeInterface_SPECIAL_VAL_NORMAL;
//#endif
//    // env->DeleteLocalRef(cls);
//  }
//}
//
#endif /* end of include guard: TAGGING_HH */
