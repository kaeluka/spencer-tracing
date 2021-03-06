/// Generates a map from id to class name 
struct proto_class_name_extractor {
  string cname = "";

  void fail() const { }

  static string description() { return __FILE__; }

  /// Event: new T where result == this
  inline bool allocate(long, const char *cname, const char *thread) {
    if (this->cname == "") this->cname = string(cname);
    return true;
  }

  inline void deallocate() {}

  /// Event: x.f = y
  ///  isDropped ==> x.f == this
  /// !isDropped ==> y == this
  inline bool newRefFromField(bool isDropped, long holder, const char *cname, const char *fname, const char *thread) {
    return true;
  }

  /// Event: x.f 
  ///   where x.f == this
  inline bool loadFromField(long holder, const char *cname, const char *fname, const char *thread) {
    return true;
  }

  /// Event: x = y or z.m(x)
  ///  isDropped ==> x == this
  /// !isDropped ==> y == this
  inline bool newRefFromVar(bool isDropped, long holder, int var, const char *cname, const char *mname, const char *thread) {
    return true;
  }

  /// Event: x
  ///   where x == this
  inline bool loadFromVar(long holder, const char *cname, const char *mname, int var, const char *thread) {
    return true;
  }

  /// Event: x.f or x.m(...)
  ///   where x == this
  inline bool useFrom(bool isModify, long obj, const char *cname, const char *mname, const char *fname, const char *thread) {
    return true;
  }

  static string classSummary() {
    return "";
  }

  string resultAsString() const {
    return this->cname;
  }
};
