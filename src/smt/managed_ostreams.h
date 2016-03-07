/*********************                                                        */
/*! \file managed_ostreams.h
 ** \verbatim
 ** Original author: Tim King
 ** Major contributors: none
 ** Minor contributors (to current version): none
 ** This file is part of the CVC4 project.
 ** Copyright (c) 2009-2016  New York University and The University of Iowa
 ** See the file COPYING in the top-level source directory for licensing
 ** information.\endverbatim
 **
 ** \brief Wrappers to handle memory management of ostreams.
 **
 ** This file contains wrappers to handle special cases of managing memory
 ** related to ostreams.
 **/

#include "cvc4_private.h"

#ifndef __CVC4__MANAGED_OSTREAMS_H
#define __CVC4__MANAGED_OSTREAMS_H

#include <ostream>

#include "options/open_ostream.h"
#include "smt/update_ostream.h"

namespace CVC4 {

/** This abstracts the management of ostream memory and initialization. */
class ManagedOstream {
 public:
  /** Initially getManagedOstream() == NULL. */
  ManagedOstream();
  virtual ~ManagedOstream();

  /** Returns the pointer to the managed ostream. */
  std::ostream* getManagedOstream() const { return d_managed; }

  /** Returns the name of the ostream geing managed. */
  virtual const char* getName() const = 0;

  /**
   * Set opens a file with filename, initializes the stream.
   * If the opened ostream is marked as managed, this calls manage(stream).
   * If the opened ostream is not marked as managed, this calls manage(NULL).
   */
  void set(const std::string& filename);

  /** If this is associated with an option, return the string value. */
  virtual std::string defaultSource() const { return ""; }

 protected:

  /**
   * Opens an ostream using OstreamOpener with the name getName() with the
   * special cases added by addSpecialCases().
   */
  std::pair<bool, std::ostream*> open(const std::string& filename) const;

  /**
   * Updates the value of managed pointer. Whenever this changes,
   * beforeRelease() is called on the old value.
   */
  void manage(std::ostream* new_managed_value);

  /** Initializes an output stream. Not necessarily managed. */
  virtual void initialize(std::ostream* outStream) {}

  /** Adds special cases to an ostreamopener. */
  virtual void addSpecialCases(OstreamOpener* opener) const {}

 private:
  std::ostream* d_managed;
}; /* class ManagedOstream */

class SetToDefaultSourceListener : public Listener {
 public:
  SetToDefaultSourceListener(ManagedOstream* managedOstream)
      : d_managedOstream(managedOstream){}

  virtual void notify() {
    d_managedOstream->set(d_managedOstream->defaultSource());
  }

 private:
  ManagedOstream* d_managedOstream;
};

/**
 * This controls the memory associated with --dump-to.
 * This is is assumed to recieve a set whenever diagnosticChannelName
 * is updated.
 */
class ManagedDumpOStream : public ManagedOstream {
 public:
  ManagedDumpOStream(){}
  ~ManagedDumpOStream();

  virtual const char* getName() const { return "dump-to"; }
  virtual std::string defaultSource() const;

 protected:
  /** Initializes an output stream. Not necessarily managed. */
  virtual void initialize(std::ostream* outStream);

  /** Adds special cases to an ostreamopener. */
  virtual void addSpecialCases(OstreamOpener* opener) const;
};/* class ManagedDumpOStream */

/**
 * When d_managedRegularChannel is non-null, it owns the memory allocated
 * with the regular-output-channel. This is set when
 * options::regularChannelName is set.
 */
class ManagedRegularOutputChannel : public ManagedOstream {
 public:
  ManagedRegularOutputChannel(){}

  /** Assumes Options are in scope. */
  ~ManagedRegularOutputChannel();

  virtual const char* getName() const { return "regular-output-channel"; }
  virtual std::string defaultSource() const;

 protected:
  /** Initializes an output stream. Not necessarily managed. */
  virtual void initialize(std::ostream* outStream);

  /** Adds special cases to an ostreamopener. */
  virtual void addSpecialCases(OstreamOpener* opener) const;
};/* class ManagedRegularOutputChannel */


/**
 * This controls the memory associated with diagnostic-output-channel.
 * This is is assumed to recieve a set whenever options::diagnosticChannelName
 * is updated.
 */
class ManagedDiagnosticOutputChannel : public ManagedOstream {
 public:
  ManagedDiagnosticOutputChannel(){}

  /** Assumes Options are in scope. */
  ~ManagedDiagnosticOutputChannel();

  virtual const char* getName() const { return "diagnostic-output-channel"; }
  virtual std::string defaultSource() const;

 protected:
  /** Initializes an output stream. Not necessarily managed. */
  virtual void initialize(std::ostream* outStream);

  /** Adds special cases to an ostreamopener. */
  virtual void addSpecialCases(OstreamOpener* opener) const;
};/* class ManagedRegularOutputChannel */

/** This controls the memory associated with replay-log. */
class ManagedReplayLogOstream : public ManagedOstream {
 public:
  ManagedReplayLogOstream();
  ~ManagedReplayLogOstream();

  std::ostream* getReplayLog() const { return d_replayLog; }
  virtual const char* getName() const { return "replay-log"; }
  virtual std::string defaultSource() const;

 protected:
  /** Initializes an output stream. Not necessarily managed. */
  virtual void initialize(std::ostream* outStream);

  /** Adds special cases to an ostreamopener. */
  virtual void addSpecialCases(OstreamOpener* opener) const;

 private:
  std::ostream* d_replayLog;
};/* class ManagedRegularOutputChannel */

}/* CVC4 namespace */

#endif /* __CVC4__MANAGED_OSTREAMS_H */