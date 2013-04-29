require File.expand_path('../fixtures/classes.rb', __FILE__)

describe "Rubinius::RespondToCache#executable" do
  before :each do
    RespondToCacheSpec::Bar.new.call_site_true
    RespondToCacheSpec::Bar.new.call_site_false

    @respond_to_cache = RespondToCacheSpec::Bar::CallSiteTrue.call_sites[0]
  end

  it "has the correct executable" do
    @respond_to_cache.executable.should == RespondToCacheSpec::Bar::CallSiteTrue
  end
end
